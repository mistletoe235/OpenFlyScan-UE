#!/usr/bin/env python3
"""Optional record-preserving oriented crop for NanoGS PLY inputs."""
from __future__ import annotations

import argparse, hashlib, json, math, os, re, sys
from pathlib import Path
from typing import Sequence
import numpy as np

SCHEMA = "nanogs.ply_crop.v1"
RESULT_SCHEMA = "nanogs.ply_crop_result.v1"
VERSION = "1.0.0"
SCALARS = {"char":("i1",1),"int8":("i1",1),"uchar":("u1",1),"uint8":("u1",1),"short":("<i2",2),"int16":("<i2",2),"ushort":("<u2",2),"uint16":("<u2",2),"int":("<i4",4),"int32":("<i4",4),"uint":("<u4",4),"uint32":("<u4",4),"float":("<f4",4),"float32":("<f4",4),"double":("<f8",8),"float64":("<f8",8)}

class CropError(RuntimeError): pass

def parse_ply(path: Path) -> dict:
    path = path.resolve(); parts=[]
    with path.open("rb") as f:
        while sum(map(len, parts)) <= 1<<20:
            line=f.readline()
            if not line: raise CropError("missing end_header")
            parts.append(line)
            if line.rstrip(b"\r\n")==b"end_header": break
    header=b"".join(parts); lines=header.decode("ascii").splitlines()
    if not lines or lines[0]!="ply" or "format binary_little_endian 1.0" not in lines: raise CropError("only binary_little_endian PLY 1.0 is supported")
    elem=None; count=None; other=0; stride=0; offsets={}; dtypes={}
    for line in lines:
        tok=line.split()
        if not tok: continue
        if tok[0]=="element":
            elem=tok[1]; n=int(tok[2])
            if elem=="vertex": count=n
            else: other+=n
        elif tok[0]=="property" and elem=="vertex":
            if len(tok)!=3 or tok[1]=="list" or tok[1] not in SCALARS: raise CropError(f"unsupported property: {line}")
            dtype,width=SCALARS[tok[1]]; offsets[tok[2]]=stride; dtypes[tok[2]]=dtype; stride+=width
    if not count or other: raise CropError("PLY must contain only a non-empty vertex element")
    if any(axis not in offsets for axis in "xyz"): raise CropError("PLY requires x/y/z")
    if len(header)+count*stride != path.stat().st_size: raise CropError("PLY size mismatch")
    return {"path":path,"header":header,"offset":len(header),"count":count,"stride":stride,"offsets":offsets,"dtypes":dtypes}

def replace_count(header: bytes, count: int) -> bytes:
    pattern=re.compile(rb"^element vertex [0-9]+$"); found=0; out=[]
    for line in header.splitlines(keepends=True):
        body=line.rstrip(b"\r\n"); ending=line[len(body):]
        if pattern.fullmatch(body):
            old_digits=body.rsplit(b" ",1)[1]; new_digits=str(count).encode()
            if len(new_digits)>len(old_digits): raise CropError("output vertex count does not fit source header")
            out.append(b"element vertex "+new_digits.rjust(len(old_digits),b"0")+ending); found+=1
        else: out.append(line)
    if found!=1: raise CropError("expected one vertex element")
    return b"".join(out)

def validate(config: dict) -> dict:
    if config.get("schema")!=SCHEMA or config.get("mode")!="oriented_rectangle": raise CropError("invalid crop schema or mode")
    for key in ("coordinate_scale","center_xy","major_axis","minor_axis","p_bounds","q_bounds"):
        value=config.get(key)
        if not isinstance(value,list) or len(value)!=2 or not all(isinstance(v,(int,float)) and math.isfinite(v) for v in value): raise CropError(f"invalid {key}")
    z_bounds=config.get("z_bounds")
    if z_bounds is not None and (not isinstance(z_bounds,list) or len(z_bounds)!=2 or not all(isinstance(v,(int,float)) and math.isfinite(v) for v in z_bounds) or z_bounds[0]>z_bounds[1]): raise CropError("invalid z_bounds")
    a=np.asarray(config["major_axis"]); b=np.asarray(config["minor_axis"])
    if abs(np.linalg.norm(a)-1)>1e-5 or abs(np.linalg.norm(b)-1)>1e-5 or abs(a@b)>1e-5: raise CropError("crop axes must be orthonormal")
    protected=config.get("protected_half_plane")
    if protected is not None and (protected.get("axis") not in ("p","q") or protected.get("side") not in ("negative","positive") or not isinstance(protected.get("value"),(int,float))): raise CropError("invalid protected_half_plane")
    return config

def load_config(path: Path) -> dict: return validate(json.loads(path.read_text(encoding="utf-8")))

def chunks(stream, layout, chunk_points):
    stream.seek(layout["offset"]); first=0
    while first<layout["count"]:
        n=min(chunk_points,layout["count"]-first); raw=stream.read(n*layout["stride"])
        if len(raw)!=n*layout["stride"]: raise CropError("truncated PLY payload")
        yield first,raw; first+=n

def values(raw, layout, name):
    return np.ndarray((len(raw)//layout["stride"],),dtype=np.dtype(layout["dtypes"][name]),buffer=raw,offset=layout["offsets"][name],strides=(layout["stride"],))

def mask(raw, layout, config):
    scale=config["coordinate_scale"]; center=config["center_xy"]
    x=values(raw,layout,"x").astype(np.float64)*scale[0]-center[0]; y=values(raw,layout,"y").astype(np.float64)*scale[1]-center[1]
    major=config["major_axis"]; minor=config["minor_axis"]
    p=x*major[0]+y*major[1]; q=x*minor[0]+y*minor[1]; pb=config["p_bounds"]; qb=config["q_bounds"]
    keep=(p>=pb[0])&(p<=pb[1])&(q>=qb[0])&(q<=qb[1]); protected=config.get("protected_half_plane")
    z_bounds=config.get("z_bounds")
    if z_bounds is not None:
        z=values(raw,layout,"z").astype(np.float64)
        keep &= (z>=z_bounds[0])&(z<=z_bounds[1])
    if protected:
        axis=p if protected["axis"]=="p" else q
        keep |= axis<=protected["value"] if protected["side"]=="negative" else axis>=protected["value"]
    return keep

def estimate(source: Path, tail=0.001, sample_points=1_000_000, chunk_points=262_144, protect="none", z_tail=0.0) -> dict:
    if not 0<=tail<0.25 or not 0<=z_tail<0.25 or sample_points<100: raise CropError("invalid automatic crop parameters")
    layout=parse_ply(source); step=max(1,math.ceil(layout["count"]/sample_points)); samples=[]
    with source.open("rb") as f:
        for first,raw in chunks(f,layout,chunk_points):
            start=(-first)%step; samples.append(np.column_stack((values(raw,layout,"x")[start::step],values(raw,layout,"y")[start::step],values(raw,layout,"z")[start::step])).astype(np.float64))
    xyz=np.concatenate(samples); xyz=xyz[np.isfinite(xyz).all(axis=1)]; xy=xyz[:,:2]; center=np.median(xy,axis=0)
    _,vec=np.linalg.eigh(np.cov(xy-center,rowvar=False)); major=vec[:,-1]
    if major[0]<0: major=-major
    minor=np.array((-major[1],major[0])); relative=xy-center; p=relative@major; q=relative@minor; lo=tail*100; hi=(1-tail)*100
    config={"schema":SCHEMA,"version":1,"mode":"oriented_rectangle","coordinate_scale":[1.0,1.0],"center_xy":center.tolist(),"major_axis":major.tolist(),"minor_axis":minor.tolist(),"p_bounds":[float(np.percentile(p,lo)),float(np.percentile(p,hi))],"q_bounds":[float(np.percentile(q,lo)),float(np.percentile(q,hi))],"z_bounds":None if z_tail==0 else [float(np.quantile(xyz[:,2],z_tail)),float(np.quantile(xyz[:,2],1-z_tail))],"protected_half_plane":None,"auto":{"tail_quantile":tail,"z_tail_quantile":z_tail,"sample_points_used":len(xy),"sample_stride":step}}
    if protect!="none": config["protected_half_plane"]={"axis":"q","side":protect.removesuffix("-q"),"value":0.0}
    return validate(config)

def scan(source: Path, config: dict, chunk_points=262_144) -> dict:
    layout=parse_ply(source); kept=0
    with source.open("rb") as f:
        for _,raw in chunks(f,layout,chunk_points): kept+=int(np.count_nonzero(mask(raw,layout,config)))
    return {"input_points":layout["count"],"output_points":kept,"removed_points":layout["count"]-kept,"removed_percent":100*(layout["count"]-kept)/layout["count"]}

def sha256(path: Path) -> str:
    digest=hashlib.sha256()
    with path.open("rb") as f:
        while data:=f.read(8<<20): digest.update(data)
    return digest.hexdigest()

def build(source: Path, output_root: Path, config: dict, force=False, chunk_points=262_144) -> dict:
    source=source.resolve(); config=validate(config); layout=parse_ply(source); source_hash=sha256(source)
    key=hashlib.sha256(json.dumps({"version":VERSION,"source":source_hash,"config":config},sort_keys=True,separators=(",",":")).encode()).hexdigest()
    directory=output_root.resolve()/source.stem/key; output=directory/(source.stem+".cropped.ply"); manifest_path=directory/"crop.json"
    if not force and output.is_file() and manifest_path.is_file(): return {"cache_hit":True,"output":str(output),"manifest":json.loads(manifest_path.read_text())}
    directory.mkdir(parents=True,exist_ok=True); partial=Path(str(output)+".partial"); kept=0
    try:
        with source.open("rb") as src, partial.open("w+b") as dst:
            dst.write(layout["header"])
            for _,raw in chunks(src,layout,chunk_points):
                records=np.frombuffer(raw,dtype=np.dtype((np.void,layout["stride"]))); selected=records[mask(raw,layout,config)]; dst.write(selected.tobytes()); kept+=len(selected)
            dst.seek(0); dst.write(replace_count(layout["header"],kept)); dst.flush(); os.fsync(dst.fileno())
        os.replace(partial,output)
    except BaseException: partial.unlink(missing_ok=True); raise
    manifest={"schema":RESULT_SCHEMA,"version":1,"tool_version":VERSION,"crop_key":key,"source":{"path":str(source),"sha256":source_hash},"output":{"path":str(output),"sha256":sha256(output)},"input_points":layout["count"],"output_points":kept,"removed_points":layout["count"]-kept,"removed_percent":100*(layout["count"]-kept)/layout["count"],"preserved_payload":"all retained vertex records copied byte-for-byte","config":config}
    temp=Path(str(manifest_path)+".partial"); temp.write_text(json.dumps(manifest,indent=2,sort_keys=True)+"\n"); os.replace(temp,manifest_path)
    return {"cache_hit":False,"output":str(output),"manifest":manifest}

def main(argv: Sequence[str]|None=None) -> int:
    parser=argparse.ArgumentParser(); sub=parser.add_subparsers(dest="command",required=True)
    for name in ("plan","build"):
        p=sub.add_parser(name); p.add_argument("source",type=Path); g=p.add_mutually_exclusive_group(required=True); g.add_argument("--config",type=Path); g.add_argument("--auto",action="store_true"); p.add_argument("--tail-quantile",type=float,default=.001); p.add_argument("--z-tail-quantile",type=float,default=0.0); p.add_argument("--sample-points",type=int,default=1_000_000); p.add_argument("--protect-half",choices=("none","negative-q","positive-q"),default="none"); p.add_argument("--chunk-points",type=int,default=262_144)
        if name=="build": p.add_argument("--output-root",type=Path,required=True); p.add_argument("--force",action="store_true")
    args=parser.parse_args(argv)
    try:
        config=load_config(args.config) if args.config else estimate(args.source,args.tail_quantile,args.sample_points,args.chunk_points,args.protect_half,args.z_tail_quantile)
        result={"config":config,**scan(args.source,config,args.chunk_points)} if args.command=="plan" else build(args.source,args.output_root,config,args.force,args.chunk_points)
        json.dump(result,sys.stdout,indent=2,sort_keys=True); print(); return 0
    except (CropError,OSError,ValueError,json.JSONDecodeError) as exc: print(f"error: {exc}",file=sys.stderr); return 2

if __name__=="__main__": raise SystemExit(main())
