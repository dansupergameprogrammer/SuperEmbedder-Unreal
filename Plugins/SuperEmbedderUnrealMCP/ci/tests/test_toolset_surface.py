"""The MCP toolset's surface, checked from source on any host (no engine needed).

The plan of record (proposal rev 9, section 2.4) names five tools and says the toolset is an optional plugin
that depends only on the engine's toolset registry (and the plugin it serves), like the SuperFAISS one. These
cells hold the source to that, and to the README's tool table, the read-only promise and the automation
registration rule (Unreal hides a test whose name is a parent of another's).

Run: python3 -m pytest Plugins/SuperEmbedderUnrealMCP/ci/tests  (or run this file directly).
"""
import json
import re
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[2]
PLUGINS = PLUGIN.parent
SOURCE = PLUGIN / "Source" / "SuperEmbedderUnrealMCP"
HEADER = SOURCE / "Public" / "SuperEmbedderToolset.h"
BODY = SOURCE / "Private" / "SuperEmbedderToolset.cpp"
TESTS = SOURCE / "Private" / "Tests" / "SuperEmbedderMCPTests.cpp"
PLAN_TOOLS = ["EncoderStatus", "EncodeText", "QueryText", "ListSets", "ValidateSets"]
ASYNC_TOOLS = {"EncodeText", "QueryText", "ValidateSets"}

COMMENTS = re.compile(r"//[^\n]*|/\*[\s\S]*?\*/")


def code(path):
    return COMMENTS.sub(" ", path.read_text(encoding="utf-8-sig"))


def tools():
    """(name, return type) for every AICallable UFUNCTION in the toolset header, in order."""
    found = []
    pattern = re.compile(
        r"UFUNCTION\s*\(\s*meta\s*=\s*\(\s*AICallable\s*\)[^)]*\)\s*static\s+([\w:<>* ]+?)\s*\b(\w+)\s*\(")
    for match in pattern.finditer(code(HEADER)):
        found.append((match[2], match[1].strip()))
    return found


def test_the_tools_are_the_plans_five_and_no_other():
    assert [name for name, _ in tools()] == PLAN_TOOLS


def test_the_asynchronous_tools_return_a_pending_result_and_the_others_a_string():
    for name, returned in tools():
        expected = "UToolCallAsyncResultString*" if name in ASYNC_TOOLS else "FString"
        assert returned.replace(" ", "") == expected, (name, returned)


def test_every_tool_is_defined_once():
    body = code(BODY)
    for name in PLAN_TOOLS:
        assert len(re.findall(r"\bUSuperEmbedderToolset::" + name + r"\s*\(", body)) == 1, name


def test_the_readme_table_lists_exactly_the_tools():
    readme = (PLUGIN / "README.md").read_text(encoding="utf-8")
    table = re.findall(r"^\| `(\w+)` \|", readme, re.M)
    assert table == PLAN_TOOLS


def test_the_plugin_is_optional_and_depends_only_on_the_registry_and_the_plugin_it_serves():
    uplugin = json.loads((PLUGIN / "SuperEmbedderUnrealMCP.uplugin").read_text(encoding="utf-8"))
    assert uplugin["EnabledByDefault"] is False
    assert [p["Name"] for p in uplugin["Plugins"]] == ["ToolsetRegistry", "SuperEmbedderUnreal"]
    (module,) = uplugin["Modules"]
    assert module["Name"] == "SuperEmbedderUnrealMCP" and module["Type"] == "Editor"
    served = json.loads((PLUGINS / "SuperEmbedderUnreal" / "SuperEmbedderUnreal.uplugin").read_text(encoding="utf-8"))
    assert module.get("PlatformAllowList") == served["Modules"][0].get("PlatformAllowList")
    assert uplugin["VersionName"] == served["VersionName"]


def test_the_module_never_depends_on_the_mcp_server_plugin():
    build = code(SOURCE / "SuperEmbedderUnrealMCP.Build.cs")
    assert '"ToolsetRegistry"' in build
    assert "ModelContextProtocol" not in build
    for path in SOURCE.rglob("*"):
        if path.suffix in {".h", ".cpp"}:
            assert "ModelContextProtocol" not in code(path), path


def test_the_tools_are_read_only():
    body = code(BODY)
    for writer in ("->Append(", "->AppendDocuments(", "->Remove(", "->Reset()", "SaveToBytes", "LoadFromBytes",
                   "SaveEncodeCache", "SavePackage", "FSuperEmbedderBake", "MarkPackageDirty"):
        if writer == "->Reset()":
            # Hold->Reset() clears the call's own handle pointer, not a set.
            assert not re.search(r"\bSet->Reset\(\)", body)
            continue
        assert writer not in body, writer


def test_registration_waits_for_post_engine_init_and_unregisters():
    module = code(SOURCE / "Private" / "SuperEmbedderUnrealMCPModule.cpp")
    assert "OnPostEngineInit" in module
    assert "RegisterToolsetClass(USuperEmbedderToolset::StaticClass())" in module
    assert "UnregisterToolsetClass(USuperEmbedderToolset::StaticClass())" in module


def automation_names(root):
    names = []
    pattern = re.compile(r'IMPLEMENT_SIMPLE_AUTOMATION_TEST\s*\(\s*\w+\s*,\s*"([^"]+)"')
    for path in sorted(root.rglob("*.cpp")):
        if "ThirdParty" not in path.parts:
            names.extend(pattern.findall(path.read_text(encoding="utf-8-sig")))
    return names


def test_automation_tests_are_executable_leaves():
    names = automation_names(SOURCE) + automation_names(PLUGINS / "SuperEmbedderUnreal" / "Source")
    assert len(automation_names(SOURCE)) == 3
    assert len(names) == len(set(names))
    parents = sorted((a, b) for a in names for b in names if b.startswith(a + "."))
    assert not parents, parents


def test_the_suite_calls_every_tool():
    tests = code(TESTS)
    for name in PLAN_TOOLS:
        assert "USuperEmbedderToolset::" + name + "(" in tests, name


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
