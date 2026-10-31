# Use ESP32 ROM flashing after repeated temporary-stub transfer failures.
Import("env")
flags = list(env.get("UPLOADERFLAGS", []))
if "write_flash" in flags:
    flags.insert(flags.index("write_flash"), "--no-stub")
    flags = [flag for flag in flags if flag != "-z"]
    env.Replace(UPLOADERFLAGS=flags)
