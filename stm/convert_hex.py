Import("env")

env.AddPostAction(
    "$BUILD_DIR/firmware.elf",
    env.VerboseAction(" ".join([
        "$OBJCOPY", "-O", "ihex",
        "$BUILD_DIR/firmware.elf",
        "$BUILD_DIR/firmware.hex"
    ]), "Building firmware.hex")
)