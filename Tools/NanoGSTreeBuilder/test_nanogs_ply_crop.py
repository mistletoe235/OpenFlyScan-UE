import json, struct, tempfile, unittest
from pathlib import Path
import nanogs_ply_crop as crop

def write_ply(path,points):
    rec=struct.Struct("<3fI"); header=("ply\nformat binary_little_endian 1.0\nelement vertex %d\nproperty float x\nproperty float y\nproperty float z\nproperty uint id\nend_header\n"%len(points)).encode(); payload=[rec.pack(*p) for p in points]; path.write_bytes(header+b"".join(payload)); return payload
def config(): return {"schema":crop.SCHEMA,"version":1,"mode":"oriented_rectangle","coordinate_scale":[1.,1.],"center_xy":[0.,0.],"major_axis":[1.,0.],"minor_axis":[0.,1.],"p_bounds":[-1.,1.],"q_bounds":[-1.,1.],"protected_half_plane":{"axis":"q","side":"negative","value":0.}}

class CropTest(unittest.TestCase):
    def test_record_preserving_and_cached(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); source=root/"in.ply"; records=write_ply(source,[(-5.,-2.,0.,1),(0.5,0.5,0.,2),(5.,2.,0.,3)])
            result=crop.build(source,root/"cache",config(),chunk_points=2); output=Path(result["output"]); layout=crop.parse_ply(output)
            self.assertEqual(layout["count"],2); self.assertEqual(output.read_bytes()[layout["offset"]:],records[0]+records[1]); self.assertTrue(crop.build(source,root/"cache",config())["cache_hit"])
    def test_auto_config(self):
        with tempfile.TemporaryDirectory() as d:
            source=Path(d)/"in.ply"; write_ply(source,[(float(i),float(i%7),0.,i) for i in range(200)])
            value=crop.estimate(source,.01,100,31,"none"); crop.validate(value); self.assertEqual(value["auto"]["sample_stride"],2)
    def test_config_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"crop.json"; path.write_text(json.dumps(config())); self.assertEqual(crop.load_config(path),config())

if __name__=="__main__": unittest.main()
