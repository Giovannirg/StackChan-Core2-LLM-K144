# Version injection script for PlatformIO
Import("env")

# You can add version info to build flags here
env.Append(CPPDEFINES=[
    ("STACKCHAN_VERSION", "\\\"1.0.0\\\""),
    ("STACKCHAN_BUILD_DATE", "\\\"" + __import__('datetime').datetime.now().strftime('%Y-%m-%d') + "\\\"")
])
