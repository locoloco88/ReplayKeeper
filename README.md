# Replay Keeper

lets you watch old league replays that would normally say "expired"

## how it works

when league updates your old replays break because the client checks the game version

this tool patches the files so the client loads them anyway

## usage

1 run it
2 it auto detects your league install path (or pick it manually)
3 make sure league client is closed
4 enable auto replace
5 open league and download your replay
6 set the patch version to match the replay you want to download

you can also download replays by game id

to play the game normally again just close the tool and restart league

## note

replays can only be downloaded up to 2 patches back

if you already have the replay file you can play any replay as long as you have the matching patch version downloaded

## files it replaces

- system.yaml (blocks version check)
- compat-version-metadata.json (spoofs the patch version)

this only lets you download old replays

to actually watch them you still need the matching old patch game files

## building

needs cmake and a c++20 compiler (built with MinGW)

```
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## contact

questions or issues hit me up on discord locoloco88
