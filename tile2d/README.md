# Tile2D - 2D tilemap game framework (semi-separated client/server)

Server thread + client thread in one process; the local client talks to the server over **shared
memory**, remote players talk over **KCP/UDP**, and the dedicated server runs with no client thread
and no graphics at all.

## Layout

    include/t2d/core      types, log, deterministic RNG, 2D math, byte streams, fixed timestep, CLI
    include/t2d/sim       tilemap (chunked, RLE, collision), tileset, world (deterministic), snapshots
    include/t2d/net       ILink, shared-memory link (mmap + lock-free SPSC rings), KCP, UDP transport, protocol
    include/t2d/client    prediction/reconciliation, snapshot interpolation, local client
    include/t2d/server    authoritative server host (own thread)
    include/t2d/render    sprite batch + tilemap renderer (built on the Ore rendering framework)
    apps/game             playable client (--mode single|host|join)
    apps/server           dedicated server (no graphics dependency)
    apps/bot              headless client used by the integration tests
    tests                 unit + integration tests (tilemap, KCP, shared memory, multi-process)

## Build

    cmake --preset debug && cmake --build build/debug && ctest --test-dir build/debug

## Run

    ./build/debug/apps/game/tile2d_game --mode single                      # single player
    ./build/debug/apps/game/tile2d_game --mode host --port 7777            # host + local player
    ./build/debug/apps/game/tile2d_game --connect 127.0.0.1:7777           # join a host
    ./build/debug/apps/server/tile2d_server --port 7777                    # dedicated server
    ./build/debug/apps/bot/tile2d_bot --connect 127.0.0.1:7777 --ticks 240 # headless client

## Verification status

* tilemap suite: 10 tests / 238 checks green
* KCP suite: 11 tests / 213 checks green (30% loss + duplication + reordering, 1 MiB transfer,
  fragmentation, RTO growth, outage recovery, zero-window probes, ikcp compatible wire format)
* shared-memory integration test: currently FAILS - the client reports a state checksum mismatch
  after applying a snapshot, i.e. the desync detector is firing. Investigate next.
* three-process test (server + two KCP bots) is implemented but slow: it is paced by wall-clock
  ticks, so it takes tens of seconds.

## Known gaps

* the desync above
* integration tests are wall-clock paced and therefore slow (a virtual clock would fix that)
* the renderer needs a display or an offscreen run; the shared memory and networking halves do not
