# Pinned tags on purpose: ":latest" has broken Wii U plugin builds before, and a
# reproducible image is worth more than being current. Bump deliberately.
FROM ghcr.io/wiiu-env/devkitppc:20260504

COPY --from=ghcr.io/wiiu-env/wiiupluginsystem:20260418 /artifacts $DEVKITPRO
COPY --from=ghcr.io/wiiu-env/libmappedmemory:20260331 /artifacts $DEVKITPRO

WORKDIR project
