"""Import the prepared active Tree source and bind it to the reusable level."""

import json
import os

import unreal


STATE_RELATIVE = "Saved/NanoGSActive/current_scene.json"
GS_TAG = "NanoGS.ActiveScene"
PLAYER_START_TAG = "NanoGS.PlayerStart"
COLLISION_FLOOR_TAG = "NanoGS.CollisionFloor"


def fail(message):
    unreal.log_error(f"NANOGS_ACTIVE_SCENE_FAILED: {message}")
    raise RuntimeError(message)


def vector(values):
    return unreal.Vector(float(values[0]), float(values[1]), float(values[2]))


def rotator(values):
    # JSON and C++ runtime use [Pitch, Yaw, Roll]. Unreal Python's positional
    # constructor is not ordered the same way, so name every field explicitly.
    return unreal.Rotator(
        pitch=float(values[0]),
        yaw=float(values[1]),
        roll=float(values[2]),
    )


def find_by_label(label):
    return [
        actor for actor in unreal.EditorLevelLibrary.get_all_level_actors()
        if actor.get_actor_label() == label
    ]


project_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
state_path = os.environ.get("NANOGS_ACTIVE_STATE", os.path.join(project_dir, STATE_RELATIVE))
state_path = os.path.normpath(state_path)
if not os.path.isfile(state_path):
    fail(f"missing prepared state: {state_path}; run prepare_active_scene.py first")
with open(state_path, "r", encoding="utf-8") as handle:
    state = json.load(handle)
if state.get("schema") != "nanogs.active_scene.state.v1":
    fail(f"unsupported state schema: {state.get('schema')}")

descriptor = os.path.normpath(os.path.join(project_dir, state["descriptor"]))
if not os.path.isfile(descriptor):
    fail(f"missing Tree descriptor: {descriptor}")
settings = state["unreal"]
source_asset_path = settings["source_asset_path"]
source_asset_directory, source_asset_name = source_asset_path.rsplit("/", 1)
task = unreal.AssetImportTask()
task.set_editor_property("filename", descriptor)
task.set_editor_property("destination_path", source_asset_directory)
task.set_editor_property("destination_name", source_asset_name)
task.set_editor_property("automated", True)
task.set_editor_property("replace_existing", True)
task.set_editor_property("save", True)
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
source = unreal.EditorAssetLibrary.load_asset(source_asset_path)
if source is None:
    fail(f"Tree source import failed: {source_asset_path}")
if not bool(source.get_editor_property("metadata_valid")):
    fail("Tree source metadata is invalid")
if int(source.get_editor_property("exact_leaf_count")) <= 0:
    fail("Tree source has no exact leaves")

target_level = settings["target_level_path"]
if not unreal.EditorAssetLibrary.does_asset_exist(target_level):
    fail(f"missing shipped runtime level: {target_level}")
unreal.EditorLevelLibrary.load_level(target_level)

actor_class = unreal.load_class(None, "/Script/NanoGS.GaussianSplatActor")
if actor_class is None:
    fail("GaussianSplatActor class unavailable")
actors = [
    actor for actor in unreal.EditorLevelLibrary.get_all_level_actors()
    if actor.get_class() == actor_class
]
labelled = [actor for actor in actors if actor.get_actor_label() == settings["actor_label"]]
if len(labelled) > 1:
    fail(f"multiple active GS actors named {settings['actor_label']}")
if labelled:
    actor = labelled[0]
elif len(actors) == 1:
    actor = actors[0]
else:
    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(actor_class, unreal.Vector(0, 0, 0))
if actor is None:
    fail("could not obtain active GaussianSplatActor")
actor.set_actor_label(settings["actor_label"])
actor.tags = list(dict.fromkeys([*actor.tags, GS_TAG]))
transform = settings["transform"]
actor.set_actor_location(vector(transform["location"]), False, False)
actor.set_actor_rotation(rotator(transform["rotation"]), False)
actor.set_actor_scale3d(vector(transform["scale"]))

component = actor.get_editor_property("gaussian_splat_component")
component.set_tree_source_asset(source)
component_settings = settings["component"]
for property_name in (
    "sh_order",
    "opacity_scale",
    "splat_scale",
    "tree_lod_splat_budget",
    "tree_lod_progressive_splat_budget",
    "tree_lod_detail_scale",
    "force_full_detail_lod0",
):
    component.set_editor_property(property_name, component_settings[property_name])
if component.get_tree_source_asset() != source:
    fail("component Tree source assignment failed")
if component.get_spatial_lod_source_asset() is not None:
    fail("component retained SpatialLOD source")
if component.get_paged_source_asset() is not None:
    fail("component retained Page v1 source")
if component.get_splat_asset() is not None:
    fail("component retained monolithic source")

player = settings["player_start"]
if player["enabled"]:
    player_start_class = unreal.load_class(None, "/Script/Engine.PlayerStart")
    if player_start_class is None:
        fail("PlayerStart class unavailable")
    player_starts = [
        candidate for candidate in unreal.EditorLevelLibrary.get_all_level_actors()
        if candidate.get_class() == player_start_class
    ]
    labelled_starts = [candidate for candidate in player_starts if candidate.get_actor_label() == player["label"]]
    player_start = labelled_starts[0] if labelled_starts else (player_starts[0] if player_starts else None)
    if player_start is None:
        player_start = unreal.EditorLevelLibrary.spawn_actor_from_class(
        player_start_class, vector(player["location"])
    )
    if player_start is None:
        fail("could not create active PlayerStart")
    for duplicate in player_starts:
        if duplicate != player_start:
            unreal.EditorLevelLibrary.destroy_actor(duplicate)
    player_start.set_actor_label(player["label"])
    player_start.tags = list(dict.fromkeys([*player_start.tags, PLAYER_START_TAG]))
    player_start.set_actor_location(vector(player["location"]), False, False)
    player_start.set_actor_rotation(rotator(player["rotation"]), False)

floor = settings["collision_floor"]
if floor["enabled"]:
    floor_actors = [
        candidate for candidate in unreal.EditorLevelLibrary.get_all_level_actors()
        if candidate.get_actor_label() == floor["label"]
        or candidate.get_actor_label().endswith("_CollisionFloor")
    ]
    labelled_floors = [candidate for candidate in floor_actors if candidate.get_actor_label() == floor["label"]]
    floor_actor = labelled_floors[0] if labelled_floors else (floor_actors[0] if floor_actors else None)
    if floor_actor is None:
        static_mesh_actor_class = unreal.load_class(None, "/Script/Engine.StaticMeshActor")
        floor_actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
            static_mesh_actor_class, vector(floor["location"])
        )
        if floor_actor is None:
            fail("could not create active collision floor")
    cube = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube.Cube")
    if cube is None:
        fail("could not load engine cube for collision floor")
    floor_component = floor_actor.get_editor_property("static_mesh_component")
    if floor_component is None:
        fail("active collision floor has no StaticMeshComponent")
    floor_component.set_static_mesh(cube)
    for duplicate in floor_actors:
        if duplicate != floor_actor:
            unreal.EditorLevelLibrary.destroy_actor(duplicate)
    floor_actor.set_actor_label(floor["label"])
    floor_actor.tags = list(dict.fromkeys([*floor_actor.tags, COLLISION_FLOOR_TAG]))
    floor_actor.set_actor_location(vector(floor["location"]), False, False)
    floor_actor.set_actor_scale3d(vector(floor["scale"]))
    floor_actor.set_actor_enable_collision(True)
    floor_actor.set_actor_hidden_in_game(bool(floor["hidden_in_game"]))
    floor_component.set_collision_profile_name("BlockAll")
    floor_component.set_collision_enabled(unreal.CollisionEnabled.QUERY_AND_PHYSICS)
    floor_component.set_editor_property("generate_overlap_events", False)
    floor_component.set_hidden_in_game(bool(floor["hidden_in_game"]))
    floor_component.set_visibility(not bool(floor.get("hidden_in_editor", True)), True)

if not unreal.EditorLevelLibrary.save_current_level():
    fail(f"could not save target level: {target_level}")
unreal.EditorAssetLibrary.save_loaded_asset(source, False)
unreal.log(
    "NANOGS_ACTIVE_SCENE_OK "
    f"level={target_level} source={source_asset_path} tree_key={state['tree_key']} "
    f"leaves={source.get_editor_property('exact_leaf_count')} "
    f"nodes={source.get_editor_property('node_count')} pages={source.get_editor_property('page_count')}"
)
