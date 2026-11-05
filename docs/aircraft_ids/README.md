# Player aircraft ids

The game keeps the player's aircraft as a number from 0 to 35 (colour variants share a number). The runtime picks the cockpit
model from that number: `ACZ_VR_COCKPIT_MODELS="id=file,..."`, which the launcher fills from `cockpits/map.txt`. An id's first
entry wins; an id without an entry gets `ACZ_VR_COCKPIT_MODEL` (the T-38). `tools/cockpit/map_check.sh` compares the launcher's
map with the last column below.

Many aircraft have no free cockpit yet and borrow the nearest one. The column says which model the public set uses today.

| id | aircraft | public cockpit |
|---|---|---|
| 0 | J35J Draken | j35j.cpit |
| 1 | Gripen C | gripen_fg.cpit |
| 2 | Typhoon | typhoon.cpit |
| 3 | Tornado GR.4 | (the T-38, until a free Tornado cockpit is built) |
| 4 | F-4E | f4e.cpit |
| 5 | F-15C | f15c.cpit |
| 6 | F-15E | f15c.cpit (stand-in) |
| 7 | F-15 S/MTD | f15c.cpit (stand-in) |
| 8 | F/A-18C | fa18c.cpit |
| 9 | EA-18G | fa18c.cpit (stand-in) |
| 10 | F-16C | f16c.cpit |
| 11 | F-16XL | f16c.cpit (stand-in) |
| 12 | F-117A | fa18c.cpit (stand-in) |
| 13 | F/A-22A | f22a.cpit |
| 14 | F-35C | f22a.cpit (stand-in) |
| 15 | F-5E | f5e_t38.cpit (stand-in) |
| 16 | F-20A | f20a.cpit |
| 17 | X-29A | f5e_t38.cpit (stand-in) |
| 18 | F-14D | f14a.cpit |
| 19 | YF-23A | (the T-38, until a free YF-23 cockpit is built) |
| 20 | EA-6B | ea6b.cpit |
| 21 | A-10A | a10a.cpit |
| 22 | Mirage 2000D | m2000d.cpit |
| 23 | Rafale M | typhoon.cpit (stand-in) |
| 24 | Su-27 | su27.cpit |
| 25 | Su-32 | su27.cpit (stand-in) |
| 26 | Su-37 | su27.cpit (stand-in) |
| 27 | Su-47 | su27.cpit (stand-in) |
| 28 | MiG-21bis | mig21bis.cpit |
| 29 | MiG-29A | mig29a.cpit |
| 30 | MiG-31 | mig29a.cpit (stand-in) |
| 31 | F-1 | f4e.cpit (stand-in) |
| 32 | F-2A | f16c.cpit (stand-in) |
| 33 | X-02 Wyvern | f22a.cpit (stand-in) |
| 34 | ADF-01 FALKEN | f22a.cpit (stand-in) |
| 35 | ADFX-01 Morgan | f22a.cpit (stand-in) |
