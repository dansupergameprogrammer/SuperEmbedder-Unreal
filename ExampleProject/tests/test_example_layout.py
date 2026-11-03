"""The example project's layout, checked from source on any host (no engine needed).

The packaging rules: an example project keeps its plugins under the repository's top-level Plugins/
folder and finds them with AdditionalPluginDirectories ["../Plugins"] ([".."] breaks the packaged game); demo
assets must be saved by stock UE 5.8, and demo folders must always be cooked. This example has no assets at all
(its scene is code on the engine's Entry map), so the cells hold it to that: no binary, the map cooked
explicitly, the model folder staged loose, and the game mode the config names defined in source.

Run: python3 -m pytest ExampleProject/tests  (or run this file directly).
"""
import json
import re
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
SOURCE = PROJECT / "Source" / "ExampleProject"


def ini(name):
    # Unreal ini files repeat keys (+Key=...), which configparser rejects; read them as lines.
    sections, current = {}, None
    for line in (PROJECT / "Config" / name).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("["):
            current = line.strip("[]")
            sections.setdefault(current, [])
        else:
            sections[current].append(line)
    return sections


def test_the_project_finds_its_plugins_in_the_top_level_plugins_folder():
    uproject = json.loads((PROJECT / "ExampleProject.uproject").read_text(encoding="utf-8"))
    assert uproject["AdditionalPluginDirectories"] == ["../Plugins"]
    assert uproject["EngineAssociation"] == "5.8"
    assert {"Name": "SuperEmbedderUnreal", "Enabled": True} in uproject["Plugins"]
    assert [m["Name"] for m in uproject["Modules"]] == ["ExampleProject"]


def test_the_example_carries_no_binary_asset():
    # Content/SuperEmbedder/ is where a checkout puts the model files (git ignores it); nothing else is content.
    model_dir = PROJECT / "Content" / "SuperEmbedder"
    binaries = [p for p in PROJECT.rglob("*") if p.suffix.lower() in {".uasset", ".umap", ".sslm"}
                and model_dir not in p.parents]
    assert not binaries, binaries
    content = PROJECT / "Content"
    assert not content.exists() or {p.name for p in content.iterdir()} <= {"SuperEmbedder"}


def test_the_map_is_the_engines_entry_map_and_is_cooked():
    engine = ini("DefaultEngine.ini")["/Script/EngineSettings.GameMapsSettings"]
    assert "GameDefaultMap=/Engine/Maps/Entry" in engine
    assert "EditorStartupMap=/Engine/Maps/Entry" in engine
    packaging = ini("DefaultGame.ini")["/Script/UnrealEd.ProjectPackagingSettings"]
    assert '+MapsToCook=(FilePath="/Engine/Maps/Entry")' in packaging


def test_the_model_folder_is_staged_loose_where_the_settings_point():
    game = ini("DefaultGame.ini")
    assert '+DirectoriesToAlwaysStageAsNonUFS=(Path="SuperEmbedder")' in game["/Script/UnrealEd.ProjectPackagingSettings"]
    settings = game["/Script/SuperEmbedderUnreal.SuperEmbedderSettings"]
    assert "ModelFile=Content/SuperEmbedder/model.sslm" in settings
    assert "TokenizerFile=Content/SuperEmbedder/tokenizer.sslm" in settings


def test_the_configured_game_mode_is_defined_in_source():
    engine = ini("DefaultEngine.ini")["/Script/EngineSettings.GameMapsSettings"]
    (line,) = [l for l in engine if l.startswith("GlobalDefaultGameMode=")]
    module, cls = re.match(r"GlobalDefaultGameMode=/Script/(\w+)\.(\w+)$", line).groups()
    assert module == "ExampleProject"
    header = (SOURCE / (cls + ".h")).read_text(encoding="utf-8")
    assert re.search(r"class A" + cls + r"\s*:\s*public AGameModeBase", header)


def test_the_example_uses_only_the_plugins_public_api():
    build = (SOURCE / "ExampleProject.Build.cs").read_text(encoding="utf-8")
    assert '"SuperEmbedderUnreal"' in build
    for path in SOURCE.rglob("*"):
        if path.suffix in {".h", ".cpp"}:
            text = path.read_text(encoding="utf-8")
            assert "Private/" not in text and "semb.h" not in text and "Core/SuperEmbedder" not in text, path


def test_the_example_drives_the_blueprint_nodes():
    body = (SOURCE / "SuperEmbedderExample.cpp").read_text(encoding="utf-8")
    for node in ("OpenSuperEmbedderEncoder(", "EncodeDocumentsAsync(", "NewVectorSet(", "AppendDocuments(",
                 "QueryTextTopKAsync("):
        assert node in body, node
    # Not the subsystem's C++ entry points: the example shows what a Blueprint graph does.
    assert "USuperEmbedderSubsystem::Get" not in body


if __name__ == "__main__":
    import sys

    failed = 0
    for name, cell in sorted(globals().items()):
        if name.startswith("test_") and callable(cell):
            try:
                cell()
                print("ok  ", name)
            except AssertionError as error:
                failed += 1
                print("FAIL", name, error)
    sys.exit(1 if failed else 0)
