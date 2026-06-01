# Replay Keeper

lets you watch old league replays that would normally say "expired"

## how it works

when league updates your old replays break because the client checks the game version

this tool patches the files so the client loads them anyway

## usage

1 run Replay Keeper
2 make sure the League and Riot Client paths are detected, or pick them manually
3 close League, or use Close Riot Processes
4 enter the patch version your replay needs and click Apply
5 enable Auto-Replace Files
6 start Riot Client and open League
7 download or watch the replay

you can also enter a GameID and use Download Replay or Watch Replay

Browse Replays lets you pick a local replay file instead of typing the GameID

to play the game normally again just close the tool and restart league

## patch downloader

use this when you need the old patch game files to actually watch a replay

1 select your region server
2 click Refresh Patches if the patch list is not loaded yet
3 select the patch version that matches the replay
4 select the language you need, usually en_US
5 choose the output folder where the patch files should be written
6 click Download Patch

## note

replays can only be downloaded up to 2 patches back

if you already have the replay file you can play any replay as long as you have the matching patch version downloaded

## files it replaces

- system.yaml (blocks version check)
- compat-version-metadata.json (spoofs the patch version)

this only lets you download old replays

to actually watch them you still need the matching old patch game files

## patch downloader credits

the patch download feature is based on the manifest parsing and download logic from [Morilli/ManifestDownloader](https://github.com/Morilli/ManifestDownloader)

ManifestDownloader is licensed under the MIT license

patch metadata is fetched from [Morilli/riot-manifests](https://github.com/Morilli/riot-manifests)

thanks to Morilli for making these projects available

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
