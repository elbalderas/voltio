"""Stand-in for the voltio CLI, so the server can be tested without a PSU.

Mimics the real CLI's argument layout and JSON output. State lives in the
JSON file named by $FAKE_VOLTIO_STATE:
  - setpoints, output state, OVP/OCP
  - "max_V"/"max_A": the model's maximum voltage/current (`max` command)
  - "load_A": current the simulated ECU draws when powered
  - "fail": if set, every command fails with this message
  - "fail_cmd": fail only commands whose "cmd args..." starts with this string
    (message: "fail" if set, else a generic one)
  - "fail_times": with "fail"/"fail_cmd", fail this many times, then succeed
    (decremented in the state file); absent = always fail
  - "omit": list of field names to delete from the `status` output
  - "calls": every command run, appended for test assertions
"""

import json
import os
import sys

DEFAULTS = {"v_set": 0.0, "i_set": 0.0, "on": False, "ovp": 60.0, "ocp": 5.0, "load_A": 0.1,
            "max_V": 60.0, "max_A": 5.0, "calls": []}


def ok(**kv):
    print(json.dumps({"status": "ok", **kv}))
    return 0


def err(msg):
    print(json.dumps({"status": "error", "message": msg}), file=sys.stderr)
    return 1


def opt(args, name):
    return float(args[args.index(name) + 1]) if name in args else None


def main(argv):
    path = os.environ["FAKE_VOLTIO_STATE"]
    try:
        with open(path) as f:
            st = {**DEFAULTS, **json.load(f)}
    except FileNotFoundError:
        st = dict(DEFAULTS)

    # Global options: --port X --dp N
    i = 0
    while i < len(argv) and argv[i].startswith("--"):
        i += 2
    cmd, args = argv[i], argv[i + 1:]
    st["calls"].append([cmd, *args])

    def save():
        with open(path, "w") as f:
            json.dump(st, f)

    if st.get("fail_cmd"):
        failing = " ".join([cmd, *args]).startswith(st["fail_cmd"])
    else:
        failing = bool(st.get("fail"))
    if failing and "fail_times" in st:
        failing = st["fail_times"] > 0
        if failing:
            st["fail_times"] -= 1
    if failing:
        save()
        return err(st.get("fail") or f"simulated failure: {st['fail_cmd']}")

    if cmd == "status":
        save()
        v = st["v_set"] if st["on"] else 0.0
        a = min(st["load_A"], st["i_set"]) if st["on"] else 0.0
        cc = st["on"] and st["load_A"] >= st["i_set"]
        out = dict(voltage_set_V=st["v_set"], current_set_A=st["i_set"],
                   voltage_actual_V=v, current_actual_A=a, power_actual_W=round(v * a, 3),
                   mode="CC" if cc else "CV")
        for name in st.get("omit", []):
            out.pop(name, None)
        return ok(**out)
    if cmd == "set":
        v, a = opt(args, "--voltage"), opt(args, "--current")
        if v is not None:
            st["v_set"] = v
        if a is not None:
            st["i_set"] = a
        save()
        return ok(voltage_set_V=st["v_set"], current_set_A=st["i_set"])
    if cmd == "output":
        st["on"] = args[0] == "on"
        save()
        return ok(output=args[0])
    if cmd == "limits":
        ovp, ocp = opt(args, "--ovp"), opt(args, "--ocp")
        if ovp is not None:
            st["ovp"] = ovp
        if ocp is not None:
            st["ocp"] = ocp
        save()
        return ok(ovp_V=st["ovp"], ocp_A=st["ocp"])
    if cmd == "max":
        save()
        return ok(max_voltage_V=st["max_V"], max_current_A=st["max_A"])
    save()
    return err("Unknown command")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
