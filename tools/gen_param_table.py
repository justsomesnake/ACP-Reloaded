"""Generates src/core/param_table_data.inc and src/core/param_ids.inc from docs/param_map.csv.

Usage (from the project root):  python -I tools/gen_param_table.py
The CSV is the source of the parameter data; the generated files are checked in
and never edited by hand.
"""
import csv
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CSV_PATH = ROOT / "docs" / "param_map.csv"
OUT_DATA = ROOT / "src" / "core" / "param_table_data.inc"
OUT_IDS = ROOT / "src" / "core" / "param_ids.inc"

TYPES = {"B": "Bool", "I": "Int", "F": "Float"}
UNITS = {"": "None", "%": "Percent", "s": "Seconds", "DPS": "Dps", "Lvl": "Level",
         "n": "Count", "enum": "Enum", "?": "Unknown"}
CONF = {"D": "NotApplicable", "G": "Game", "X": "ExeLabel", "C": "Community", "A": "Assumed", "?": "Unknown"}
CATS = {"HG": "HeroGeneral", "HF": "HeroFighting", "NG": "NpcGeneral", "NF": "NpcFighting", "W": "World"}
GROUPS = {
    "Vital": "Vitality", "Movement": "Movement", "WaterHeat": "Water & Heat",
    "Progress": "Progression", "Gear": "Equipment", "Character": "Character & Companions",
    "Damage": "Damage", "Assassination": "Assassination", "Adrenaline": "Adrenaline",
    "Desync": "Desynchronization", "Perception": "Perception", "LevelScale": "Level Scaling",
    "Combat": "Combat", "RelPlayer": "Relations to Player", "RelFactions": "Faction Relations",
    "Time": "Time of Day", "Sandstorm": "Sandstorm", "Map": "World Map",
    "Vehicles": "Vehicles & Siege", "Environment": "Environment & Loot", "PostFx": "Game Look (Post-Processing)",
    "CustomFx": "Custom Filter (ACP Reloaded)",
}
# Tooltip explanations, based on analysis of the game code.
HINTS = {
    "PlayerMaxLevelEnabled": "Level cap: once you reach the level below, you gain no more XP. Does not grant levels.",
    "PlayerMaxLevel": "Level at which XP gain stops (only with the level cap enabled). Never raises or lowers your level.",
    "CanAcquireAllSkills": "Unlocks the whole skill tree: any ability can be bought without its prerequisites. Ability points are still required.",
    "MaxPurchasableSkillsNumberEnabled": "Skill cap: limits how many abilities can be bought in total.",
    "MaxPurchasableSkillsNumber": "Maximum number of abilities (only with the skill cap enabled). Removes nothing already owned.",
    "PlayerExperienceGainingMultiplier": "XP gain in percent. 100 is the maximum - this can only reduce XP gain.",
    "PlayableCharacterIndex": "Character you play as. Switches immediately.",
    "DayPeriodTimeModifier": "Higher = slower time. 100% = normal, 5% = 20x faster, 400% = 4x slower. Applies immediately.",
    "DayPeriodsMorningDuration": "Real-time minutes for the morning (4-8 h), scaled by 'Length of the day'.",
    "DayPeriodsMiddayDuration": "Real-time minutes for midday (8-16 h), scaled by 'Length of the day'.",
    "DayPeriodsAfternoonDuration": "Real-time minutes for the afternoon (16-20 h), scaled by 'Length of the day'.",
    "DayPeriodsNightDuration": "Real-time minutes for the night (20-4 h), scaled by 'Length of the day'.",
    "PostProcessFilterSettings": "One of eleven built-in looks (colour filter, saturation, tint).",
    "PostProcessFilterEnabled": "Enables the game's built-in look selected below.",
    "DesyncOnAssassinationEnabled": "May have no effect in the current game version (no reader found).",
    "DesyncOnKillEnabled": "May have no effect in the current game version (no reader found).",
    "WorldMapUnfogged": "Removes the fog on the world map. Reportedly cannot be undone in this save.",
    "PostProcessCustomFilmGrain": "Film grain. Rendered by ACP Reloaded (the game ignores this value).",
    "PostProcessCustomVignette": "Darkens the screen edges. Rendered by ACP Reloaded.",
    "PostProcessCustomBrightness": "Brightness multiplier. Rendered by ACP Reloaded.",
    "PostProcessCustomTemperature": "Colour temperature: negative = cooler, positive = warmer. Rendered by ACP Reloaded.",
    "PostProcessCustomTint": "Tint: negative = magenta, positive = green. Rendered by ACP Reloaded.",
    "PostProcessCustomSaturation": "Saturation: -100 = black and white. Rendered by ACP Reloaded.",
}

# Original labels from the EXE where they differ from the key.
LABELS = {
    "PostProcessCustomFilmGrain": "Film Grain",
    "PostProcessCustomVignette": "Vignette",
    "PostProcessCustomBrightness": "Brightness",
    "PostProcessCustomTemperature": "Temperature",
    "PostProcessCustomTint": "Tint",
    "PostProcessCustomSaturation": "Saturation",
    "PostProcessFilterSettings": "Look",
    "PlayableCharacterIndex": "Playable Character",
    "AssassinationEnabled": "Assassination Enable",
    "NPCHurtboxScaleFactor": "Hurtbox Scale Factor",
    "SandstormBaseProbability": "Base Probability",
    "SandstormSpeedFactor": "Speed Factor",
    "SandstormCooldownFactor": "Cooldown Factor",
    "SandstormLifetimeFactor": "Lifetime Factor",
    "DayPeriodTimeModifier": "Length of the day",
    "DayPeriodsMorningDuration": "Morning (4-8 h), minutes",
    "DayPeriodsMiddayDuration": "Midday (8-16 h), minutes",
    "DayPeriodsAfternoonDuration": "Afternoon (16-20 h), minutes",
    "DayPeriodsNightDuration": "Night (20-4 h), minutes",
    "DesyncInSandstormEnabled": "Desynchronization in sandstorm",
    "DesyncOnAssassinationEnabled": "Desynchronization after assassinations",
    "DesyncOnKillEnabled": "Desynchronization after kills",
    "DesyncOnAssassinationNumber": "Number of assassinations before desync",
    "DesyncOnKillNumber": "Number of kills before desync",
    "DesyncOnAssassinationCooldownTime": "Cooldown time before reset (sec)",
    "DesyncOnKillCooldownTime": "Cooldown time before reset (sec)",
    "PlayerDamageInSandstormInterval": "Player damage interval (sec)",
    "NPCDamageInSandstormInterval": "NPC damage interval (sec)",
    "PlayerDamageInSandstormLightDPS": "Player light zone DPS",
    "PlayerDamageInSandstormHeavyDPS": "Player heavy zone DPS",
    "PlayerDamageInSandstormInsideDPS": "Player center zone DPS",
    "NPCDamageInSandstormLightDPS": "NPC light zone DPS",
    "NPCDamageInSandstormHeavyDPS": "NPC heavy zone DPS",
    "NPCDamageInSandstormInsideDPS": "NPC center zone DPS",
    "PostProcessFilterEnabled": "Post Process Filter",
}


def auto_label(key: str) -> str:
    s = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", key)
    s = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", " ", s)
    return s


def value_literal(type_code: str, text: str) -> str:
    if type_code == "B":
        return f"ParamValue::Bool({'true' if text == 'true' else 'false'})"
    if type_code == "I":
        return f"ParamValue::Int({int(text)})"
    return f"ParamValue::Float({float(text)!r}f)"


def cpp_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    with CSV_PATH.open(newline="", encoding="ascii") as f:
        rows = list(csv.DictReader(f, delimiter=";"))
    keys = [r["Key"] for r in rows]
    if len(keys) != len(set(keys)):
        print("Duplicate keys in the CSV", file=sys.stderr)
        return 1
    index = {k: i for i, k in enumerate(keys)}

    ids = ["// Generated by tools/gen_param_table.py from docs/param_map.csv - do not edit.\n"]
    data = ["// Generated by tools/gen_param_table.py from docs/param_map.csv - do not edit.\n"]
    for r in rows:
        k, t = r["Key"], r["Type"]
        ids.append(f"{k},\n")
        conds = []
        if r["DisabledWhen"]:
            for part in r["DisabledWhen"].split("||"):
                m = re.fullmatch(r"(\w+)==(true|false)", part.strip())
                if not m or m.group(1) not in index or rows[index[m.group(1)]]["Type"] != "B":
                    print(f"Invalid condition for {k}: {part}", file=sys.stderr)
                    return 1
                conds.append(f"DisabledWhen{{ParamId::{m.group(1)}, {m.group(2)}}}")
        while len(conds) < 2:
            conds.append("DisabledWhen{}")
        if t == "B":
            lo, hi = "ParamValue::Bool(false)", "ParamValue::Bool(true)"
        else:
            lo, hi = value_literal(t, r["Min"]), value_literal(t, r["Max"])
        data.append(
            "    ParamDesc{"
            f"ParamId::{k}, {cpp_str(k)}, {cpp_str(LABELS.get(k, auto_label(k)))}, "
            f"{cpp_str(GROUPS[r['Group']])}, ParamType::{TYPES[t]}, Category::{CATS[r['Cat']]}, "
            f"Unit::{UNITS[r['Unit']]}, Confidence::{CONF[r['Conf']]}, "
            f"{value_literal(t, r['Default'])}, {lo}, {hi}, "
            f"{{{conds[0]}, {conds[1]}}}, {'true' if r.get('ReadOnly') == '1' else 'false'}, "
            f"{'true' if r.get('Reload') == '1' else 'false'}, {'true' if r.get('Local') == '1' else 'false'}, "
            f"{cpp_str(HINTS.get(k, ''))}"
            "},\n"
        )
    OUT_IDS.write_text("".join(ids), encoding="ascii", newline="\n")
    OUT_DATA.write_text("".join(data), encoding="ascii", newline="\n")
    print(f"{len(rows)} parameters written.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
