#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from typing import Any, Iterable, Sequence


SPIRV_MAGIC = 0x07230203
SPIRV_HEADER_WORDS = 5
SPIRV_OP_ENTRY_POINT = 15
SPIRV_OP_EXECUTION_MODE = 16
SPIRV_OP_VARIABLE = 59
SPIRV_OP_DECORATE = 71
SPIRV_EXECUTION_MODE_EARLY_FRAGMENT_TESTS = 9
SPIRV_DECORATION_SPEC_ID = 1
SPIRV_DECORATION_BUILT_IN = 11
SPIRV_DECORATION_LOCATION = 30
SPIRV_DECORATION_BINDING = 33
SPIRV_DECORATION_DESCRIPTOR_SET = 34
SPIRV_BUILT_IN_SAMPLE_MASK = 20
SPIRV_STORAGE_CLASS_INPUT = 1
SPIRV_STORAGE_CLASS_OUTPUT = 3
SPIRV_EXECUTION_MODELS = {"vertex": 0, "pixel": 4}
SHADER_OVERRIDE_STAGES = {"pixel": 0, "vertex": 1}
SHADER_OVERRIDE_ACTIVATIONS = {"stage": 0, "pipeline_pair": 1}
SUPPORTED_BUILT_INS = {"SampleMask": SPIRV_BUILT_IN_SAMPLE_MASK}
SPEC_CONSTANT_ALPHA_TEST = 0x00000002
USE_STOCK_TEXTURE_MASK = 0xFFFFFFFF
HASH_PATTERN = re.compile(r"0x[0-9A-Fa-f]{16}\Z")
MASK_PATTERN = re.compile(r"0x[0-9A-Fa-f]{8}\Z")
ENTRY_POINT_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
DEFINE_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:=[A-Za-z0-9_+.,-]+)?\Z")
DESCRIPTOR_BINDING_PATTERN = re.compile(r"([0-9]+):([0-9]+)\Z")
SUPPORTED_SAMPLE_COUNTS = frozenset((1, 2, 4, 8, 16, 32, 64))
TOP_LEVEL_KEYS = {"version", "overrides"}
ENTRY_KEYS = {
    "stage",
    "hash",
    "language",
    "source",
    "entry_point",
    "specialization_constants_mask",
    "defines",
    "activation",
    "pipeline_pair_id",
    "counterpart_hashes",
    "used_texture_mask",
    "support_radius_bits",
    "required_builtins",
    "require_early_fragment_tests",
    "supported_sample_counts",
    "expected_input_locations",
    "expected_output_locations",
    "expected_descriptor_bindings",
    "expected_spec_ids",
}


class ManifestError(ValueError):
    pass


@dataclass(frozen=True)
class SpirvInterface:
    input_locations: tuple[int, ...]
    output_locations: tuple[int, ...]
    descriptor_bindings: tuple[tuple[int, int], ...]
    spec_ids: tuple[int, ...]
    builtins: tuple[int, ...]


def _parse_uint_list(
    raw_value: Any, context: str, *, allowed: frozenset[int] | None = None
) -> tuple[int, ...]:
    if not isinstance(raw_value, list) or any(
        not isinstance(value, int) or isinstance(value, bool) or value < 0 or value > 0xFFFFFFFF
        for value in raw_value
    ):
        raise ManifestError(f"{context} must be an array of uint32 values")
    if len(set(raw_value)) != len(raw_value):
        raise ManifestError(f"{context} contains a duplicate")
    if allowed is not None and any(value not in allowed for value in raw_value):
        rendered = ", ".join(str(value) for value in sorted(allowed))
        raise ManifestError(f"{context} may contain only: {rendered}")
    return tuple(sorted(raw_value))


def _parse_optional_uint_list(
    raw_entry: dict[str, Any], key: str, context: str
) -> tuple[int, ...] | None:
    if key not in raw_entry:
        return None
    return _parse_uint_list(raw_entry[key], f"{context}.{key}")


def _parse_optional_descriptor_bindings(
    raw_entry: dict[str, Any], key: str, context: str
) -> tuple[tuple[int, int], ...] | None:
    if key not in raw_entry:
        return None
    raw_value = raw_entry[key]
    if not isinstance(raw_value, list):
        raise ManifestError(f"{context}.{key} must be an array of SET:BINDING strings")
    result: list[tuple[int, int]] = []
    for index, value in enumerate(raw_value):
        if not isinstance(value, str):
            raise ManifestError(f"{context}.{key}[{index}] must be a SET:BINDING string")
        match = DESCRIPTOR_BINDING_PATTERN.fullmatch(value)
        if not match:
            raise ManifestError(f"{context}.{key}[{index}] must be a SET:BINDING string")
        result.append((int(match.group(1)), int(match.group(2))))
    if len(set(result)) != len(result):
        raise ManifestError(f"{context}.{key} contains a duplicate")
    return tuple(sorted(result))


def _require_exact_keys(value: dict[str, Any], allowed: set[str], context: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise ManifestError(f"{context} contains unknown keys: {', '.join(unknown)}")


def _parse_hex(value: Any, pattern: re.Pattern[str], context: str) -> int:
    if not isinstance(value, str) or not pattern.fullmatch(value):
        raise ManifestError(f"{context} must match {pattern.pattern!r}")
    return int(value, 16)


def _resolve_source(source_root: Path, source_value: Any, context: str) -> tuple[Path, str]:
    if not isinstance(source_value, str) or not source_value:
        raise ManifestError(f"{context}.source must be a non-empty relative path")
    relative = Path(source_value)
    if relative.is_absolute():
        raise ManifestError(f"{context}.source must be relative to the source root")
    root = source_root.resolve(strict=True)
    source = (root / relative).resolve(strict=False)
    try:
        canonical_relative = source.relative_to(root).as_posix()
    except ValueError as error:
        raise ManifestError(f"{context}.source escapes the source root") from error
    if not source.is_file():
        raise ManifestError(f"{context}.source does not exist: {canonical_relative}")
    return source, canonical_relative


def load_manifest(manifest_path: Path, source_root: Path) -> list[dict[str, Any]]:
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(f"failed to read manifest {manifest_path}: {error}") from error
    if not isinstance(document, dict):
        raise ManifestError("manifest root must be an object")
    _require_exact_keys(document, TOP_LEVEL_KEYS, "manifest")
    if document.get("version") != 1:
        raise ManifestError("manifest.version must be 1")
    raw_entries = document.get("overrides")
    if not isinstance(raw_entries, list):
        raise ManifestError("manifest.overrides must be an array")

    entries: list[dict[str, Any]] = []
    identities: set[tuple[int, int]] = set()
    for index, raw_entry in enumerate(raw_entries):
        context = f"manifest.overrides[{index}]"
        if not isinstance(raw_entry, dict):
            raise ManifestError(f"{context} must be an object")
        _require_exact_keys(raw_entry, ENTRY_KEYS, context)

        stage_name = raw_entry.get("stage")
        if stage_name not in SHADER_OVERRIDE_STAGES:
            raise ManifestError(f"{context}.stage must be 'pixel' or 'vertex'")
        language = raw_entry.get("language")
        if language not in {"hlsl", "glsl"}:
            raise ManifestError(f"{context}.language must be 'hlsl' or 'glsl'")
        source, source_name = _resolve_source(source_root, raw_entry.get("source"), context)
        expected_suffix = f".{language}"
        if source.suffix.lower() != expected_suffix:
            raise ManifestError(f"{context}.source must use the {expected_suffix} extension")

        entry_point = raw_entry.get("entry_point")
        if not isinstance(entry_point, str) or not ENTRY_POINT_PATTERN.fullmatch(entry_point):
            raise ManifestError(f"{context}.entry_point is not a valid shader entry point")
        if language == "glsl" and entry_point != "main":
            raise ManifestError(f"{context}.entry_point must be 'main' for GLSL")
        shader_hash = _parse_hex(raw_entry.get("hash"), HASH_PATTERN, f"{context}.hash")
        mask = _parse_hex(
            raw_entry.get("specialization_constants_mask"),
            MASK_PATTERN,
            f"{context}.specialization_constants_mask",
        )

        raw_defines = raw_entry.get("defines", [])
        if not isinstance(raw_defines, list) or any(
            not isinstance(define, str) or not DEFINE_PATTERN.fullmatch(define)
            for define in raw_defines
        ):
            raise ManifestError(f"{context}.defines must be an array of NAME or NAME=value strings")
        if len(set(raw_defines)) != len(raw_defines):
            raise ManifestError(f"{context}.defines contains a duplicate")

        activation_name = raw_entry.get("activation", "stage")
        if activation_name not in SHADER_OVERRIDE_ACTIVATIONS:
            raise ManifestError(f"{context}.activation must be 'stage' or 'pipeline_pair'")
        activation = SHADER_OVERRIDE_ACTIVATIONS[activation_name]

        raw_pair_id = raw_entry.get("pipeline_pair_id", 0)
        if not isinstance(raw_pair_id, int) or isinstance(raw_pair_id, bool):
            raise ManifestError(f"{context}.pipeline_pair_id must be an integer")
        if raw_pair_id < 0 or raw_pair_id > 0xFFFFFFFF:
            raise ManifestError(f"{context}.pipeline_pair_id is outside uint32 range")

        raw_counterparts = raw_entry.get("counterpart_hashes", [])
        if not isinstance(raw_counterparts, list):
            raise ManifestError(f"{context}.counterpart_hashes must be an array")
        counterpart_hashes = [
            _parse_hex(value, HASH_PATTERN, f"{context}.counterpart_hashes[{item_index}]")
            for item_index, value in enumerate(raw_counterparts)
        ]
        if len(set(counterpart_hashes)) != len(counterpart_hashes):
            raise ManifestError(f"{context}.counterpart_hashes contains a duplicate")

        if activation_name == "pipeline_pair":
            if raw_pair_id == 0:
                raise ManifestError(f"{context}.pipeline_pair_id must be nonzero for pipeline_pair")
            if not counterpart_hashes:
                raise ManifestError(f"{context}.counterpart_hashes must not be empty for pipeline_pair")
        elif raw_pair_id or counterpart_hashes:
            raise ManifestError(
                f"{context} may only declare pipeline_pair_id/counterpart_hashes for pipeline_pair"
            )

        used_texture_mask = USE_STOCK_TEXTURE_MASK
        if "used_texture_mask" in raw_entry:
            used_texture_mask = _parse_hex(
                raw_entry["used_texture_mask"], MASK_PATTERN, f"{context}.used_texture_mask"
            )

        support_radius_bits = 0
        if "support_radius_bits" in raw_entry:
            support_radius_bits = _parse_hex(
                raw_entry["support_radius_bits"], MASK_PATTERN, f"{context}.support_radius_bits"
            )
            support_radius = struct.unpack("<f", struct.pack("<I", support_radius_bits))[0]
            if not math.isfinite(support_radius) or support_radius <= 0.0:
                raise ManifestError(f"{context}.support_radius_bits must encode a finite positive float")

        raw_required_builtins = raw_entry.get("required_builtins", [])
        if not isinstance(raw_required_builtins, list) or any(
            not isinstance(name, str) or name not in SUPPORTED_BUILT_INS
            for name in raw_required_builtins
        ):
            raise ManifestError(
                f"{context}.required_builtins must contain only: {', '.join(SUPPORTED_BUILT_INS)}"
            )
        if len(set(raw_required_builtins)) != len(raw_required_builtins):
            raise ManifestError(f"{context}.required_builtins contains a duplicate")
        if raw_required_builtins and stage_name != "pixel":
            raise ManifestError(f"{context}.required_builtins is only valid for pixel shaders")

        require_early_fragment_tests = raw_entry.get("require_early_fragment_tests", False)
        if not isinstance(require_early_fragment_tests, bool):
            raise ManifestError(f"{context}.require_early_fragment_tests must be a boolean")
        if require_early_fragment_tests and stage_name != "pixel":
            raise ManifestError(
                f"{context}.require_early_fragment_tests is only valid for pixel shaders"
            )

        supported_sample_counts = _parse_uint_list(
            raw_entry.get("supported_sample_counts", []),
            f"{context}.supported_sample_counts",
            allowed=SUPPORTED_SAMPLE_COUNTS,
        )
        supported_sample_count_mask = 0
        for sample_count in supported_sample_counts:
            supported_sample_count_mask |= sample_count
        if activation_name == "pipeline_pair" and not supported_sample_count_mask:
            raise ManifestError(
                f"{context}.supported_sample_counts must not be empty for pipeline_pair"
            )

        expected_input_locations = _parse_optional_uint_list(
            raw_entry, "expected_input_locations", context
        )
        expected_output_locations = _parse_optional_uint_list(
            raw_entry, "expected_output_locations", context
        )
        expected_descriptor_bindings = _parse_optional_descriptor_bindings(
            raw_entry, "expected_descriptor_bindings", context
        )
        expected_spec_ids = _parse_optional_uint_list(raw_entry, "expected_spec_ids", context)

        identity = (SHADER_OVERRIDE_STAGES[stage_name], shader_hash)
        if identity in identities:
            raise ManifestError(
                f"{context} duplicates the {stage_name} shader identity {raw_entry['hash']}"
            )
        identities.add(identity)
        entries.append(
            {
                "stage_name": stage_name,
                "stage": SHADER_OVERRIDE_STAGES[stage_name],
                "execution_model": SPIRV_EXECUTION_MODELS[stage_name],
                "hash": shader_hash,
                "language": language,
                "source": source,
                "source_name": source_name,
                "entry_point": entry_point,
                "specialization_constants_mask": mask,
                "defines": sorted(raw_defines),
                "activation_name": activation_name,
                "activation": activation,
                "pipeline_pair_id": raw_pair_id,
                "counterpart_hashes": sorted(counterpart_hashes),
                "used_texture_mask": used_texture_mask,
                "support_radius_bits": support_radius_bits,
                "required_builtins": tuple(sorted(raw_required_builtins)),
                "require_early_fragment_tests": require_early_fragment_tests,
                "supported_sample_counts": supported_sample_counts,
                "supported_sample_count_mask": supported_sample_count_mask,
                "expected_input_locations": expected_input_locations,
                "expected_output_locations": expected_output_locations,
                "expected_descriptor_bindings": expected_descriptor_bindings,
                "expected_spec_ids": expected_spec_ids,
            }
        )

    entries_by_pair: dict[int, list[dict[str, Any]]] = {}
    for entry in entries:
        if entry["activation_name"] == "pipeline_pair":
            entries_by_pair.setdefault(entry["pipeline_pair_id"], []).append(entry)

    for pair_id, pair_entries in sorted(entries_by_pair.items()):
        context = f"pipeline_pair_id {pair_id}"
        if len(pair_entries) != 2:
            raise ManifestError(f"{context} must contain exactly one vertex and one pixel override")
        by_stage = {entry["stage_name"]: entry for entry in pair_entries}
        if set(by_stage) != {"vertex", "pixel"}:
            raise ManifestError(f"{context} must contain exactly one vertex and one pixel override")
        vertex = by_stage["vertex"]
        pixel = by_stage["pixel"]
        if pixel["hash"] not in vertex["counterpart_hashes"]:
            raise ManifestError(f"{context} vertex override does not permit its pixel counterpart")
        if vertex["hash"] not in pixel["counterpart_hashes"]:
            raise ManifestError(f"{context} pixel override does not permit its vertex counterpart")
        if not vertex["support_radius_bits"] or not pixel["support_radius_bits"]:
            raise ManifestError(f"{context} must declare support_radius_bits on both overrides")
        vertex_radius = struct.unpack("<f", struct.pack("<I", vertex["support_radius_bits"]))[0]
        pixel_radius = struct.unpack("<f", struct.pack("<I", pixel["support_radius_bits"]))[0]
        if vertex_radius < pixel_radius:
            raise ManifestError(
                f"{context} vertex support radius must contain the pixel nonzero support radius"
            )
        if vertex["supported_sample_count_mask"] != pixel["supported_sample_count_mask"]:
            raise ManifestError(f"{context} shader stages must support the same sample counts")
        if not vertex["supported_sample_count_mask"]:
            raise ManifestError(f"{context} must support at least one sample count")

    return sorted(entries, key=lambda entry: (entry["stage"], entry["hash"]))


def _run_tool(command: Sequence[str], environment: dict[str, str] | None = None) -> None:
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=environment,
    )
    if result.returncode:
        rendered = " ".join(command)
        raise ManifestError(f"shader tool failed ({rendered}):\n{result.stdout}")


def _compiler_environment(library_path: str | None) -> dict[str, str]:
    environment = dict(os.environ)
    if not library_path:
        return environment
    variable = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
    existing = environment.get(variable)
    environment[variable] = (
        library_path if not existing else os.pathsep.join((library_path, existing))
    )
    return environment


def requires_late_fragment_tests(entry: dict[str, Any]) -> bool:
    return (
        entry["stage_name"] == "pixel"
        and entry["specialization_constants_mask"] & SPEC_CONSTANT_ALPHA_TEST
    ) != 0


def runtime_entry_point(entry: dict[str, Any]) -> str:
    # Vulkan title pipelines use shaderMain for both stock and override modules.
    # GLSL source still declares main; rename only the exported SPIR-V entry.
    return "shaderMain" if entry["language"] == "glsl" else entry["entry_point"]


def compile_shader(
    entry: dict[str, Any],
    output_path: Path,
    source_root: Path,
    dxc: str | None,
    dxc_library_path: str | None,
    glslang_validator: str | None,
    additional_defines: Sequence[str] = (),
) -> None:
    source = entry["source"]
    if entry["language"] == "hlsl":
        if not dxc:
            raise ManifestError(f"DXC is required by {entry['source_name']}")
        target = "vs_6_0" if entry["stage_name"] == "vertex" else "ps_6_0"
        command = [
            dxc,
            "-spirv",
            "-fspv-target-env=vulkan1.0",
            "-HV",
            "2021",
            "-T",
            target,
            "-E",
            entry["entry_point"],
            "-all-resources-bound",
            "-fvk-use-dx-layout",
            "-O3",
            "-Qstrip_debug",
            "-WX",
            "-I",
            str(source_root),
            "-I",
            str(source.parent),
        ]
        if entry["stage_name"] == "vertex":
            command.append("-fvk-invert-y")
        else:
            # Match XenosRecomp's stock pixel-shader compilation. Its shared
            # sampling helpers select implicit derivatives only with this
            # stage define; without it, overrides silently force mip level 0.
            command.append("-DXENOS_RECOMP_PIXEL_SHADER")
        command.extend(f"-D{define}" for define in entry["defines"])
        command.extend(f"-D{define}" for define in additional_defines)
        command.extend(("-Fo", str(output_path), str(source)))
        _run_tool(command, _compiler_environment(dxc_library_path))
        return

    if not glslang_validator:
        raise ManifestError(f"glslangValidator is required by {entry['source_name']}")
    shader_stage = "vert" if entry["stage_name"] == "vertex" else "frag"
    command = [
        glslang_validator,
        "-V",
        "--target-env",
        "vulkan1.0",
        "-S",
        shader_stage,
        "--source-entrypoint",
        entry["entry_point"],
        "-e",
        runtime_entry_point(entry),
        "-g0",
        # No -Os: glslang 16's size passes drop unused push-constant members,
        # which renumbers the shared-address member the native translator needs.
        "-I" + str(source_root),
        "-I" + str(source.parent),
    ]
    for define in entry["defines"]:
        command.extend(("--define-macro", define))
    for define in additional_defines:
        command.extend(("--define-macro", define))
    command.extend(("-o", str(output_path), str(source)))
    _run_tool(command)


def _decode_spirv_string(words: Sequence[int]) -> tuple[str, int]:
    encoded = b"".join(struct.pack("<I", word) for word in words)
    terminator = encoded.find(b"\0")
    if terminator < 0:
        raise ManifestError("SPIR-V OpEntryPoint name is not null-terminated")
    try:
        value = encoded[:terminator].decode("utf-8")
    except UnicodeDecodeError as error:
        raise ManifestError("SPIR-V OpEntryPoint name is not UTF-8") from error
    consumed_words = (terminator + 1 + struct.calcsize("<I") - 1) // struct.calcsize("<I")
    return value, consumed_words


def validate_spirv(
    blob: bytes,
    expected_execution_model: int,
    expected_entry_point: str,
    forbid_early_fragment_tests: bool = False,
    required_builtins: Sequence[str] = (),
    require_early_fragment_tests: bool = False,
    expected_input_locations: tuple[int, ...] | None = None,
    expected_output_locations: tuple[int, ...] | None = None,
    expected_descriptor_bindings: tuple[tuple[int, int], ...] | None = None,
    expected_spec_ids: tuple[int, ...] | None = None,
) -> tuple[tuple[int, ...], SpirvInterface]:
    if len(blob) < SPIRV_HEADER_WORDS * struct.calcsize("<I"):
        raise ManifestError("SPIR-V module is shorter than its header")
    if len(blob) % struct.calcsize("<I"):
        raise ManifestError("SPIR-V byte size is not a multiple of four")
    word_count = len(blob) // struct.calcsize("<I")
    words = struct.unpack(f"<{word_count}I", blob)
    if words[0] != SPIRV_MAGIC:
        raise ManifestError("SPIR-V module has an invalid magic number")

    matching_entry_points: set[int] = set()
    matching_interfaces: set[int] = set()
    early_fragment_test_entries: set[int] = set()
    storage_class_by_id: dict[int, int] = {}
    builtin_by_id: dict[int, int] = {}
    location_by_id: dict[int, int] = {}
    binding_by_id: dict[int, int] = {}
    descriptor_set_by_id: dict[int, int] = {}
    spec_id_by_id: dict[int, int] = {}
    index = SPIRV_HEADER_WORDS
    while index < len(words):
        instruction = words[index]
        instruction_word_count = instruction >> 16
        opcode = instruction & 0xFFFF
        if not instruction_word_count:
            raise ManifestError("SPIR-V instruction has a zero word count")
        instruction_end = index + instruction_word_count
        if instruction_end > len(words):
            raise ManifestError("SPIR-V instruction extends past the module")
        if opcode == SPIRV_OP_ENTRY_POINT:
            if instruction_word_count < 4:
                raise ManifestError("SPIR-V OpEntryPoint is truncated")
            execution_model = words[index + 1]
            entry_point_id = words[index + 2]
            entry_point, name_word_count = _decode_spirv_string(
                words[index + 3 : instruction_end]
            )
            if execution_model == expected_execution_model and entry_point == expected_entry_point:
                matching_entry_points.add(entry_point_id)
                interface_start = index + 3 + name_word_count
                matching_interfaces.update(words[interface_start:instruction_end])
        elif opcode == SPIRV_OP_EXECUTION_MODE and instruction_word_count >= 3:
            if words[index + 2] == SPIRV_EXECUTION_MODE_EARLY_FRAGMENT_TESTS:
                early_fragment_test_entries.add(words[index + 1])
        elif opcode == SPIRV_OP_VARIABLE and instruction_word_count >= 4:
            storage_class_by_id[words[index + 2]] = words[index + 3]
        elif opcode == SPIRV_OP_DECORATE and instruction_word_count >= 3:
            target_id = words[index + 1]
            decoration = words[index + 2]
            if instruction_word_count >= 4:
                value = words[index + 3]
                if decoration == SPIRV_DECORATION_SPEC_ID:
                    spec_id_by_id[target_id] = value
                elif decoration == SPIRV_DECORATION_BUILT_IN:
                    builtin_by_id[target_id] = value
                elif decoration == SPIRV_DECORATION_LOCATION:
                    location_by_id[target_id] = value
                elif decoration == SPIRV_DECORATION_BINDING:
                    binding_by_id[target_id] = value
                elif decoration == SPIRV_DECORATION_DESCRIPTOR_SET:
                    descriptor_set_by_id[target_id] = value
        index = instruction_end

    if not matching_entry_points:
        raise ManifestError(
            f"SPIR-V does not contain the requested stage/entry point {expected_entry_point!r}"
        )
    has_early_fragment_tests = bool(matching_entry_points & early_fragment_test_entries)
    if forbid_early_fragment_tests and has_early_fragment_tests:
        raise ManifestError("late SPIR-V unexpectedly enables EarlyFragmentTests")
    if require_early_fragment_tests and not has_early_fragment_tests:
        raise ManifestError("SPIR-V entry point is missing required EarlyFragmentTests")

    interface_builtins = {
        builtin_by_id[interface_id]
        for interface_id in matching_interfaces
        if interface_id in builtin_by_id
    }
    for builtin_name in required_builtins:
        builtin = SUPPORTED_BUILT_INS[builtin_name]
        if builtin not in interface_builtins:
            raise ManifestError(
                f"SPIR-V entry point is missing required BuiltIn {builtin_name}"
            )

    interface = SpirvInterface(
        input_locations=tuple(
            sorted(
                location_by_id[interface_id]
                for interface_id in matching_interfaces
                if storage_class_by_id.get(interface_id) == SPIRV_STORAGE_CLASS_INPUT
                and interface_id in location_by_id
            )
        ),
        output_locations=tuple(
            sorted(
                location_by_id[interface_id]
                for interface_id in matching_interfaces
                if storage_class_by_id.get(interface_id) == SPIRV_STORAGE_CLASS_OUTPUT
                and interface_id in location_by_id
            )
        ),
        descriptor_bindings=tuple(
            sorted(
                (descriptor_set_by_id[target_id], binding_by_id[target_id])
                for target_id in storage_class_by_id
                if target_id in descriptor_set_by_id and target_id in binding_by_id
            )
        ),
        spec_ids=tuple(sorted(spec_id_by_id.values())),
        builtins=tuple(sorted(interface_builtins)),
    )

    expected_fields = (
        ("input locations", expected_input_locations, interface.input_locations),
        ("output locations", expected_output_locations, interface.output_locations),
        ("descriptor bindings", expected_descriptor_bindings, interface.descriptor_bindings),
        ("specialization IDs", expected_spec_ids, interface.spec_ids),
    )
    for field_name, expected, actual in expected_fields:
        if expected is not None and expected != actual:
            raise ManifestError(
                f"SPIR-V {field_name} mismatch: expected {expected}, found {actual}"
            )
    return words, interface


def _format_words(words: Iterable[int]) -> str:
    return "\n".join(f"    0x{word:08X}u," for word in words)


def generate_cpp(compiled_entries: Sequence[dict[str, Any]]) -> str:
    output = [
        '#include <shader_overrides/shader_override_cache.h>',
        "",
        "namespace {",
    ]
    for index, entry in enumerate(compiled_entries):
        if entry["counterpart_hashes"]:
            output.extend(
                (
                    f"constexpr uint64_t kShaderOverrideCounterparts{index}[] = {{",
                    "\n".join(
                        f"    0x{counterpart_hash:016X}ull,"
                        for counterpart_hash in entry["counterpart_hashes"]
                    ),
                    "};",
                    "",
                )
            )
        output.extend(
            (
                f"constexpr uint32_t kShaderOverrideSpirv{index}[] = {{",
                _format_words(entry["words"]),
                "};",
                "",
            )
        )
        if entry.get("late_words") is not None:
            output.extend(
                (
                    f"constexpr uint32_t kShaderOverrideLateSpirv{index}[] = {{",
                    _format_words(entry["late_words"]),
                    "};",
                    "",
                )
            )
    output.append("}  // namespace")
    output.append("")
    if compiled_entries:
        output.append("const ShaderOverrideCacheEntry g_shaderOverrideEntries[] = {")
        for index, entry in enumerate(compiled_entries):
            stage_constant = (
                "kShaderOverrideStageVertex"
                if entry["stage_name"] == "vertex"
                else "kShaderOverrideStagePixel"
            )
            filename = json.dumps(entry["source_name"], ensure_ascii=True)
            if entry.get("late_words") is None:
                late_spirv = "nullptr"
                late_spirv_size = "0"
            else:
                late_spirv = f"kShaderOverrideLateSpirv{index}"
                late_spirv_size = f"sizeof(kShaderOverrideLateSpirv{index})"
            activation_constant = (
                "kShaderOverrideActivationPipelinePair"
                if entry["activation_name"] == "pipeline_pair"
                else "kShaderOverrideActivationStage"
            )
            if entry["counterpart_hashes"]:
                counterparts = f"kShaderOverrideCounterparts{index}"
                counterpart_count = (
                    f"sizeof(kShaderOverrideCounterparts{index}) / "
                    f"sizeof(kShaderOverrideCounterparts{index}[0])"
                )
            else:
                counterparts = "nullptr"
                counterpart_count = "0"
            output.append(
                "    {"
                f"0x{entry['hash']:016X}ull, {stage_constant}, "
                f"0x{entry['specialization_constants_mask']:08X}u, "
                f"{activation_constant}, {entry['pipeline_pair_id']}u, "
                f"0x{entry['used_texture_mask']:08X}u, "
                f"0x{entry['support_radius_bits']:08X}u, "
                f"0x{entry['supported_sample_count_mask']:08X}u, "
                f"{counterparts}, {counterpart_count}, "
                f"kShaderOverrideSpirv{index}, sizeof(kShaderOverrideSpirv{index}), "
                f"{late_spirv}, {late_spirv_size}, {filename}"
                "},"
            )
        output.append("};")
        output.append(
            "const size_t g_shaderOverrideEntryCount = "
            "sizeof(g_shaderOverrideEntries) / sizeof(g_shaderOverrideEntries[0]);"
        )
    else:
        output.extend(
            (
                "const ShaderOverrideCacheEntry g_shaderOverrideEntries[1] = {};",
                "const size_t g_shaderOverrideEntryCount = 0;",
            )
        )
    output.append("")
    return "\n".join(output)


def _write_if_changed(path: Path, content: str) -> None:
    encoded = content.encode("utf-8")
    if path.is_file() and path.read_bytes() == encoded:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(encoded)
    temporary.replace(path)


def _escape_depfile_path(path: Path) -> str:
    return str(path).replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ")


def generate_depfile(output: Path, dependencies: Sequence[Path]) -> str:
    target = _escape_depfile_path(output.resolve())
    dependency_list = " ".join(
        _escape_depfile_path(dependency.resolve()) for dependency in dependencies
    )
    return f"{target}: {dependency_list}\n"


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compile hash-specific GTA IV native shader overrides into a C++ SPIR-V table."
    )
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--depfile", type=Path)
    parser.add_argument("--dxc")
    parser.add_argument("--dxc-library-path")
    parser.add_argument("--glslang-validator")
    parser.add_argument("--spirv-val")
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    try:
        entries = load_manifest(options.manifest, options.source_root)
        if entries and not options.spirv_val:
            raise ManifestError("spirv-val is required when the override manifest is not empty")

        compiled_entries: list[dict[str, Any]] = []
        with tempfile.TemporaryDirectory(prefix="gta4-shader-overrides-") as temporary_directory:
            temporary_root = Path(temporary_directory)
            for index, entry in enumerate(entries):
                spirv_path = temporary_root / f"override-{index}.spv"
                compile_shader(
                    entry,
                    spirv_path,
                    options.source_root.resolve(strict=True),
                    options.dxc,
                    options.dxc_library_path,
                    options.glslang_validator,
                )
                _run_tool(
                    (
                        options.spirv_val,
                        "--target-env",
                        "vulkan1.0",
                        str(spirv_path),
                    )
                )
                compiled_entry = dict(entry)
                compiled_entry["words"], early_interface = validate_spirv(
                    spirv_path.read_bytes(),
                    entry["execution_model"],
                    runtime_entry_point(entry),
                    required_builtins=entry["required_builtins"],
                    require_early_fragment_tests=entry["require_early_fragment_tests"],
                    expected_input_locations=entry["expected_input_locations"],
                    expected_output_locations=entry["expected_output_locations"],
                    expected_descriptor_bindings=entry["expected_descriptor_bindings"],
                    expected_spec_ids=entry["expected_spec_ids"],
                )
                compiled_entry["late_words"] = None
                if requires_late_fragment_tests(entry):
                    late_spirv_path = temporary_root / f"override-{index}-late.spv"
                    compile_shader(
                        entry,
                        late_spirv_path,
                        options.source_root.resolve(strict=True),
                        options.dxc,
                        options.dxc_library_path,
                        options.glslang_validator,
                        ("XENOS_RECOMP_LATE_FRAGMENT_TESTS",),
                    )
                    _run_tool(
                        (
                            options.spirv_val,
                            "--target-env",
                            "vulkan1.0",
                            str(late_spirv_path),
                        )
                    )
                    compiled_entry["late_words"], late_interface = validate_spirv(
                        late_spirv_path.read_bytes(),
                        entry["execution_model"],
                        runtime_entry_point(entry),
                        forbid_early_fragment_tests=True,
                        required_builtins=entry["required_builtins"],
                        expected_input_locations=entry["expected_input_locations"],
                        expected_output_locations=entry["expected_output_locations"],
                        expected_descriptor_bindings=entry["expected_descriptor_bindings"],
                        expected_spec_ids=entry["expected_spec_ids"],
                    )
                    if early_interface != late_interface:
                        raise ManifestError(
                            f"{entry['source_name']} early/late SPIR-V interfaces differ: "
                            f"early={early_interface}, late={late_interface}"
                        )
                compiled_entries.append(compiled_entry)

        _write_if_changed(options.output, generate_cpp(compiled_entries))
        if options.depfile:
            dependencies = [options.manifest, Path(__file__)]
            dependencies.extend(entry["source"] for entry in entries)
            _write_if_changed(options.depfile, generate_depfile(options.output, dependencies))
    except ManifestError as error:
        print(f"shader override compiler: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
