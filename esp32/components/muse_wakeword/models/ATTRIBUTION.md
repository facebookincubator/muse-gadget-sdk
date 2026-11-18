<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Hey Muse model: training data attribution

`hey_muse.pte` was trained only on public-domain, CC0 and CC BY material.
It contains no recordings: these datasets were used to train it.

> Trained on synthetic speech generated with Piper (GPL-3.0-or-later; used
> only as a tool, nothing of it ships) using
> [rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices) (MIT)
> at revision `6b9cfaacb4fb2e02a9621227e3c1a48bec9710c3`, including new
> voices made by blending speakers of `en_US-libritts-high`. Voice training
> data: [LibriTTS](https://www.openslr.org/60/) (CC BY 4.0);
> [Hi-Fi Multi-Speaker English TTS](https://www.openslr.org/109/) (CC BY 4.0);
> the CSTR Alba corpus (University of Edinburgh, CC BY 4.0);
> [LibriVox](https://librivox.org) recordings and
> [LJ Speech](https://keithito.com/LJ-Speech-Dataset/) (public domain);
> OHF-Voice voice datasets (CC0). Accent variants were phonemized with
> espeak-ng (GPL-3.0; tool only).
>
> Non-wake speech, backgrounds and reverberation:
> [LibriSpeech](https://www.openslr.org/12/) (CC BY 4.0);
> [AMI Meeting Corpus](https://groups.inf.ed.ac.uk/ami/corpus/) (CC BY 4.0);
> [VoxPopuli](https://github.com/facebookresearch/voxpopuli) (CC0; source:
> European Parliament); [Multilingual LibriSpeech](https://www.openslr.org/94/)
> (CC BY 4.0); [The People's Speech](https://mlcommons.org/datasets/peoples-speech/)
> (MLCommons, CC-BY subset only);
> [BUT Speech@FIT Reverb Database](https://speech.fit.vutbr.cz/software/but-speech-fit-reverb-database)
> (Szöke et al., IEEE JSTSP 2019, CC BY 4.0); [MUSAN](https://www.openslr.org/17/)
> (Snyder, Chen and Povey, 2015; only its public-domain and CC BY files, credited
> below); synthetic room impulse responses made with pyroomacoustics (MIT).

Piper voices used: `en_US-libritts-high`, `en_US-hifi_tts_92-medium`,
`en_GB-cori-high`, `en_GB-cori-medium`, `en_GB-alba-medium`,
`en_US-kathleen-medium`, `en_US-kristin-medium`, `en_US-john-medium`,
`en_US-ljspeech-medium`, `en_US-ljspeech-high`, `en_US-joe-medium` and
`en_US-mike-medium`. None of them is derived from the lessac voice.

## MUSAN files under CC BY

Credits as MUSAN's per-directory `LICENSE` files give them. Public-domain
MUSAN files were used too and need no credit.

### music/rfm: Kevin MacLeod ([incompetech.com](https://incompetech.com)), CC BY 3.0

| File | Work |
|---|---|
| music-rfm-0000 | "8bit Dungeon Boss" (by Kevin MacLeod) |
| music-rfm-0001 | "Airport Lounge" (by Kevin MacLeod) |
| music-rfm-0003 | "Ambler" (by Kevin MacLeod) |
| music-rfm-0004 | "As I Figure" (by Kevin MacLeod) |
| music-rfm-0005 | "Babylon" (by Kevin MacLeod) |
| music-rfm-0006 | "Backed Vibes" (by Kevin MacLeod) |
| music-rfm-0008 | "Bass Vibes" (by Kevin MacLeod) |
| music-rfm-0009 | "Bass Walker" (by Kevin MacLeod) |
| music-rfm-0010 | "Beach Bum" (by Kevin MacLeod) |
| music-rfm-0011 | "Bet You Can ver 2" (by Kevin MacLeod) |
| music-rfm-0013 | "Blipotron" (by Kevin MacLeod) |
| music-rfm-0014 | "Blue Scorpion" (by Kevin MacLeod) |
| music-rfm-0015 | "Breakdown" (by Kevin MacLeod) |
| music-rfm-0016 | "Broken Reality" (by Kevin MacLeod) |
| music-rfm-0018 | "Chipper Doodle" (by Kevin MacLeod) |
| music-rfm-0019 | "Cipher2" (by Kevin MacLeod) |
| music-rfm-0020 | "Club Diver" (by Kevin MacLeod) |
| music-rfm-0021 | "Cold Funk" (by Kevin MacLeod) |
| music-rfm-0023 | "Cool Hard Facts" (by Kevin MacLeod) |
| music-rfm-0024 | "Cool Vibes" (by Kevin MacLeod) |
| music-rfm-0025 | "Corncob" (by Kevin MacLeod) |
| music-rfm-0026 | "Covert Affair" (by Kevin MacLeod) |
| music-rfm-0028 | "Cut and Run" (by Kevin MacLeod) |
| music-rfm-0029 | "Cut Trance" (by Kevin MacLeod) |
| music-rfm-0030 | "Dance Monster" (by Kevin MacLeod) |
| music-rfm-0031 | "Dances and Dames" (by Kevin MacLeod) |
| music-rfm-0033 | "Dead Drop" (by Kevin MacLeod) |
| music-rfm-0034 | "Deliberate Thought" (by Kevin MacLeod) |
| music-rfm-0035 | "Dirt Rhodes" (by Kevin MacLeod) |
| music-rfm-0036 | "Dispersion Relation" (by Kevin MacLeod) |
| music-rfm-0038 | "EDM Detection Mode" (by Kevin MacLeod) |
| music-rfm-0039 | "Eighties Action" (by Kevin MacLeod) |
| music-rfm-0040 | "Electrodoodle" (by Kevin MacLeod) |
| music-rfm-0041 | "Electro Sketch" (by Kevin MacLeod) |
| music-rfm-0043 | "Exhilarate" (by Kevin MacLeod) |
| music-rfm-0044 | "Exit the Premises" (by Kevin MacLeod) |
| music-rfm-0045 | "Faster Does It" (by Kevin MacLeod) |
| music-rfm-0046 | "Feelin Good" (by Kevin MacLeod) |
| music-rfm-0048 | "Gearhead" (by Kevin MacLeod) |
| music-rfm-0049 | "George Street Shuffle" (by Kevin MacLeod) |
| music-rfm-0050 | "Go Cart" (by Kevin MacLeod) |
| music-rfm-0051 | "Groove Grove" (by Kevin MacLeod) |
| music-rfm-0053 | "Happy Bee" (by Kevin MacLeod) |
| music-rfm-0054 | "Hep Cats" (by Kevin MacLeod) |
| music-rfm-0055 | "Highlight Reel" (by Kevin MacLeod) |
| music-rfm-0056 | "Hot Swing" (by Kevin MacLeod) |
| music-rfm-0058 | "Hustle" (by Kevin MacLeod) |
| music-rfm-0059 | "Hypnothis" (by Kevin MacLeod) |
| music-rfm-0060 | "Ice Flow" (by Kevin MacLeod) |
| music-rfm-0061 | "I Feel You" (by Kevin MacLeod) |
| music-rfm-0063 | "Iron Bacon" (by Kevin MacLeod) |
| music-rfm-0064 | "Itty Bitty 8 Bit" (by Kevin MacLeod) |
| music-rfm-0065 | "Jaunty Gumption" (by Kevin MacLeod) |
| music-rfm-0066 | "Kick Shock" (by Kevin MacLeod) |
| music-rfm-0068 | "Klockworx" (by Kevin MacLeod) |
| music-rfm-0069 | "Kool Kats" (by Kevin MacLeod) |
| music-rfm-0070 | "Last Kiss Goodnight" (by Kevin MacLeod) |
| music-rfm-0071 | "Latin Industries" (by Kevin MacLeod) |
| music-rfm-0073 | "Lightless Dawn" (by Kevin MacLeod) |
| music-rfm-0074 | "Local Forecast" (by Kevin MacLeod) |
| music-rfm-0075 | "Long Stroll" (by Kevin MacLeod) |
| music-rfm-0076 | "Long Time Coming" (by Kevin MacLeod) |
| music-rfm-0078 | "Matt's Blues" (by Kevin MacLeod) |
| music-rfm-0079 | "Mellowtron" (by Kevin MacLeod) |
| music-rfm-0080 | "Mining by Moonlight" (by Kevin MacLeod) |
| music-rfm-0081 | "Misuse" (by Kevin MacLeod) |
| music-rfm-0083 | "Motivator" (by Kevin MacLeod) |
| music-rfm-0084 | "Move Forward" (by Kevin MacLeod) |
| music-rfm-0085 | "MTA" (by Kevin MacLeod) |
| music-rfm-0086 | "Neolith" (by Kevin MacLeod) |
| music-rfm-0088 | "New Friendly" (by Kevin MacLeod) |
| music-rfm-0089 | "Nile's Blues" (by Kevin MacLeod) |
| music-rfm-0090 | "No Good Layabout" (by Kevin MacLeod) |
| music-rfm-0091 | "One Sly Move" (by Kevin MacLeod) |
| music-rfm-0093 | "Opportunity Walks" (by Kevin MacLeod) |
| music-rfm-0094 | "Ouroboros" (by Kevin MacLeod) |
| music-rfm-0095 | "Pamgaea" (by Kevin MacLeod) |
| music-rfm-0096 | "Phat Sketch" (by Kevin MacLeod) |
| music-rfm-0098 | "Pinball Spring" (by Kevin MacLeod) |
| music-rfm-0099 | "Porch Blues" (by Kevin MacLeod) |
| music-rfm-0100 | "Presenterator" (by Kevin MacLeod) |
| music-rfm-0101 | "Pump Sting" (by Kevin MacLeod) |
| music-rfm-0103 | "Raw" (by Kevin MacLeod) |
| music-rfm-0104 | "Ready Aim Fire" (by Kevin MacLeod) |
| music-rfm-0105 | "Reformat" (by Kevin MacLeod) |
| music-rfm-0106 | "RetroFuture Clean" (by Kevin MacLeod) |
| music-rfm-0108 | "Riptide" (by Kevin MacLeod) |
| music-rfm-0109 | "Rocket" (by Kevin MacLeod) |
| music-rfm-0110 | "Rock on Chicago" (by Kevin MacLeod) |
| music-rfm-0111 | "Rock Over London" (by Kevin MacLeod) |
| music-rfm-0113 | "Sax Rock and Roll" (by Kevin MacLeod) |
| music-rfm-0114 | "Secrets of the Schoolyard" (by Kevin MacLeod) |
| music-rfm-0115 | "Severe Tire Damage" (by Kevin MacLeod) |
| music-rfm-0116 | "Shiny Tech2" (by Kevin MacLeod) |
| music-rfm-0118 | "Sidewalk Shade" (by Kevin MacLeod) |
| music-rfm-0119 | "Silly Fun" (by Kevin MacLeod) |
| music-rfm-0120 | "Slow Burn" (by Kevin MacLeod) |
| music-rfm-0121 | "Sock Hop" (by Kevin MacLeod) |
| music-rfm-0123 | "Spacial Harvest" (by Kevin MacLeod) |
| music-rfm-0124 | "Summon the Rawk" (by Kevin MacLeod) |
| music-rfm-0125 | "Sunflower Dance Party" (by Kevin MacLeod) |
| music-rfm-0126 | "Super Friendly" (by Kevin MacLeod) |
| music-rfm-0128 | "Sweeter Vermouth" (by Kevin MacLeod) |
| music-rfm-0129 | "Take the Lead" (by Kevin MacLeod) |
| music-rfm-0130 | "Tech Talk" (by Kevin MacLeod) |
| music-rfm-0131 | "Theme for Harold var 3" (by Kevin MacLeod) |
| music-rfm-0133 | "Thinking Out Loud" (by Kevin MacLeod) |
| music-rfm-0134 | "Tranquility" (by Kevin MacLeod) |
| music-rfm-0135 | "Trips It" (by Kevin MacLeod) |
| music-rfm-0136 | "Twisted" (by Kevin MacLeod) |
| music-rfm-0138 | "Unwritten Return" (by Kevin MacLeod) |
| music-rfm-0139 | "Vegas Glitz" (by Kevin MacLeod) |
| music-rfm-0140 | "Vibe Ace" (by Kevin MacLeod) |
| music-rfm-0141 | "Video Dungeon Crawl" (by Kevin MacLeod) |
| music-rfm-0143 | "Welcome to the Show" (by Kevin MacLeod) |
| music-rfm-0144 | "Whiskey on the Mississippi" (by Kevin MacLeod) |
| music-rfm-0145 | "Winter Reflections" (by Kevin MacLeod) |
| music-rfm-0146 | "Zap Beat" (by Kevin MacLeod) |

### music/jamendo, CC BY 3.0

| File | Work |
|---|---|
| music-jamendo-0042 | "infradrums". by Bod; Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a85814/male-magnetics |
| music-jamendo-0043 | "komodovegan". by Bod; Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a85814/male-magnetics |
| music-jamendo-0045 | "morphium drill". by Bod; Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a85814/male-magnetics |
| music-jamendo-0046 | "optical spasm". by Bod; Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a85814/male-magnetics |
| music-jamendo-0101 | "DIRECTO AL SOL". album by José Carrillo; Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a137608/directo-al-sol |
| music-jamendo-0102 | "EL HOMBRE FELIZ". album by José Carrillo; Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a137608/directo-al-sol |
| music-jamendo-0103 | "EN SILENCO". album by José Carrillo; Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a137608/directo-al-sol |
| music-jamendo-0105 | "MACANUDO". album by José Carrillo; Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a137608/directo-al-sol |
| music-jamendo-0106 | "COMO UN ARBOL VIEJO". album by José Carrillo; Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a137608/directo-al-sol |
| music-jamendo-0136 | "Deeper For You". Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a144705/what-is-love |
| music-jamendo-0137 | "Lets Start Again". Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a144705/what-is-love |
| music-jamendo-0138 | "Madly, Deeply". Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a144705/what-is-love |
| music-jamendo-0140 | "Open". Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a144705/what-is-love |
| music-jamendo-0141 | "What Is Love". Attribution 3.0 Unported (CC BY 3.0); https://www.jamendo.com/en/list/a144705/what-is-love |

### noise/sound-bible, CC BY 3.0

| File | Work |
|---|---|
| noise-sound-bible-0001 | Title: Answering Machine Beep; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/5-Answering-Machine-Beep.html |
| noise-sound-bible-0003 | Title: Audience Applause; License: Attribution 3.0; Recorded by Matthiew11; License: Attribution 3.0; http://soundbible.com/2087-Audience-Applause.html |
| noise-sound-bible-0005 | Title: Background Noise; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/531-Background-Noise.html |
| noise-sound-bible-0008 | Title: Busy Signal; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1072-Busy-Signal.html |
| noise-sound-bible-0009 | Title: Buzz Fade Out; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1198-Buzz-Fade-Out.html |
| noise-sound-bible-0010 | Title: Buzz; License: Attribution 3.0; Recorded by KP; http://soundbible.com/1581-Buzz.html |
| noise-sound-bible-0011 | Title: Canadian Geese Migration; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1861-Canadian-Geese-Migration.html |
| noise-sound-bible-0013 | Title: Cat Meow 2; License: Attribution 3.0; Recorded by Cat Stevens; http://soundbible.com/1954-Cat-Meow-2.html |
| noise-sound-bible-0014 | Title: Cicada; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/920-Cicada.html |
| noise-sound-bible-0016 | Title: City Centre; License: Attribution 3.0; Recorded by Hopeinawe; http://soundbible.com/2000-City-Centre.html |
| noise-sound-bible-0018 | Title: UFO Landing; License: Attribution 3.0; Recorded by Sonidor; http://soundbible.com/2063-UFO-Landing.html |
| noise-sound-bible-0019 | Title: Dialing Phone Number; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/418-Dialing-Phone-Number.html |
| noise-sound-bible-0020 | Title: Dial Tone; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1070-Dial-Tone.html |
| noise-sound-bible-0023 | Title: Eat Chips; License: Attribution 3.0; Recorded by Caroline Ford; http://soundbible.com/912-Eat-Chips.html |
| noise-sound-bible-0024 | Title: Eating; License: Attribution 3.0; Recorded by Caroline Ford; http://soundbible.com/976-Eating.html |
| noise-sound-bible-0026 | Title: Eye Poke; License: Attribution 3.0; Recorded by Klocko; http://soundbible.com/2048-Eye-Poke.html |
| noise-sound-bible-0028 | Title: Fax Machine; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/851-Fax-Machine.html |
| noise-sound-bible-0030 | Title: Fire Truck Siren; License: Attribution 3.0; Recorded by FiremanSam; http://soundbible.com/1511-Fire-Truck-Siren.html |
| noise-sound-bible-0031 | Title: Football Crowd; License: Attribution 3.0; Recorded by GoGo; http://soundbible.com/1882-Football-Crowd.html |
| noise-sound-bible-0033 | Title: Frying Chicken; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1482-Frying-Chicken.html |
| noise-sound-bible-0034 | Title: Gun Battle Sound; About: Guns bullet flybysricochets distant artillery explosions; License: Attribution 3.0; Recorded by ReamProductions; http://soundbible.com/2078-Gun-Battle-Sound.html |
| noise-sound-bible-0036 | Title: Helicopter Approach; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/2074-Helicopter-Approach.html |
| noise-sound-bible-0038 | Title: Home Phone Ringing; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/315-Home-Phone-Ringing.html |
| noise-sound-bible-0040 | Title: Large Chains; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1101-Large-Chains.html |
| noise-sound-bible-0041 | Title: Mallard Duck Quacking; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1862-Mallard-Duck-Quacking.html |
| noise-sound-bible-0046 | Title: MP5 SMG 9mm; License: Attribution 3.0; Recorded by gunguru; http://soundbible.com/2091-MP5-SMG-9mm.html |
| noise-sound-bible-0048 | Title: Night Rain In Russian City; License: Attribution 3.0; Recorded by Alexander Tram; http://soundbible.com/2059-Night-Rain-In-Russian-City.html |
| noise-sound-bible-0050 | Title: Page Turn; License: Attribution 3.0; Recorded by Mark DiAngelo; http://soundbible.com/2066-Page-Turn.html |
| noise-sound-bible-0051 | Title: Pen Clicking; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/835-Pen-Clicking.html |
| noise-sound-bible-0053 | Title: Phone Off Hook; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1091-Phone-Off-Hook.html |
| noise-sound-bible-0054 | Title: Phone Ringing 8x; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1869-Phone-Ringing-8x.html |
| noise-sound-bible-0055 | Title: Phone Ringing; License: Attribution 3.0; Recorded by acclivity; http://soundbible.com/1518-Phone-Ringing.html |
| noise-sound-bible-0056 | Title: Phone Vibrating; License: Attribution 3.0; Recorded by Sound Explorer; http://soundbible.com/1994-Phone-Vibrating.html |
| noise-sound-bible-0058 | Title: Pin Dropping; License: Attribution 3.0; Recorded by Brian Rocca; http://soundbible.com/1992-Pin-Dropping.html |
| noise-sound-bible-0060 | Title: Railroad Crossing Bell; License: Attribution 3.0; Recorded by Brylon Terry; http://soundbible.com/2070-Railroad-Crossing-Bell.html |
| noise-sound-bible-0061 | Title: Rain Inside House; License: Attribution 3.0; Recorded by Mark DiAngelo; http://soundbible.com/2065-Rain-Inside-House.html |
| noise-sound-bible-0063 | Title: Record Player Static; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1674-Record-Player-Static.html |
| noise-sound-bible-0064 | Title: Rockslide Small; License: Attribution 3.0; Recorded by Sound Explorer; http://soundbible.com/1987-Rockslide-Small.html |
| noise-sound-bible-0065 | Title: RPG Plus Shrapnel; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/2075-RPG-Plus-Shrapnel.html |
| noise-sound-bible-0066 | Title: Rustle; License: Attribution 3.0; Recorded by Caroline Ford; http://soundbible.com/895-Rustle.html |
| noise-sound-bible-0068 | Title: Sharpen Knife; License: Attribution 3.0; Recorded by J Bravo; http://soundbible.com/2025-Sharpen-Knife.html |
| noise-sound-bible-0069 | Title: Shooting Star; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1744-Shooting-Star.html |
| noise-sound-bible-0070 | Title: Short Dial Tone; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1118-Short-Dial-Tone.html |
| noise-sound-bible-0071 | Title: Silly Farts; Recorded by Joe; License: Attribution 3.0; http://soundbible.com/2088-Silly-Farts.html |
| noise-sound-bible-0073 | Title: Sqeaking Door; License: Attribution 3.0; Recorded by Sarasprella; http://soundbible.com/1870-Sqeaking-Door.html |
| noise-sound-bible-0076 | Title: Telephone Ring; License: Attribution 3.0; Recorded by transitking; http://soundbible.com/1411-Telephone-Ring.html |
| noise-sound-bible-0078 | Title: Thunder Sound FX; License: Attribution 3.0; Recorded by Grant Evans; http://soundbible.com/2053-Thunder-Sound-FX.html |
| noise-sound-bible-0079 | Title: Thunder Strike 1; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/2015-Thunder-Strike-1.html |
| noise-sound-bible-0080 | Title: Triangle Dinner Bell; License: Attribution 3.0; Recorded by CowBoyTyrone; http://soundbible.com/1525-Triangle-Dinner-Bell.html |
| noise-sound-bible-0081 | Title: Truck Idle And Pull Off; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1116-Truck-Idle-And-Pull-Off.html |
| noise-sound-bible-0083 | Title: Winchester 1873 Single Shots; License: Attribution 3.0; Recorded by Grant Evans; http://soundbible.com/2071-Winchester-1873-Single-Shots.html |
| noise-sound-bible-0085 | Title: Woop Woop; License: Attribution 3.0; Recorded by Mike Koenig; http://soundbible.com/1377-Woop-Woop.html |
| noise-sound-bible-0086 | Title: Woosh; License: Attribution 3.0; Recorded by Mark DiAngelo; http://soundbible.com/2068-Woosh.html |

### music/fma and music/fma-western-art

| File | Statement |
|---|---|
| music-fma-wa-0044 | wcfsymphony In Concert by Jason Weinberger & wcfsymphony is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0046 | wcfsymphony In Concert by Jason Weinberger & wcfsymphony is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0056 | Classical Sampler by Kevin MacLeod is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0058 | Classical Sampler by Kevin MacLeod is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0059 | Classical Sampler by Kevin MacLeod is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0061 | Classical Sampler by Kevin MacLeod is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0062 | Classical Sampler by Kevin MacLeod is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0064 | Classical Sampler by Kevin MacLeod is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0066 | Classical Sampler by Kevin MacLeod is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0067 | Classical Sampler by Kevin MacLeod is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0068 | Classical Sampler by Kevin MacLeod is licensed under a Creative Commons Attribution License. |
| music-fma-wa-0084 | Stabat Mater - Domenico Scarlatti by The Tudor Consort is licensed under a Attribution 3.0 International License. |
| music-fma-wa-0086 | Stabat Mater - Domenico Scarlatti by The Tudor Consort is licensed under a Attribution 3.0 International License. |
| music-fma-wa-0087 | Stabat Mater - Domenico Scarlatti by The Tudor Consort is licensed under a Attribution 3.0 International License. |
| music-fma-wa-0088 | Stabat Mater - Domenico Scarlatti by The Tudor Consort is licensed under a Attribution 3.0 International License. |
| music-fma-wa-0091 | Stabat Mater - Domenico Scarlatti by The Tudor Consort is licensed under a Attribution 3.0 International License. |
| music-fma-wa-0092 | Stabat Mater - Domenico Scarlatti by The Tudor Consort is licensed under a Attribution 3.0 International License. |
| music-fma-0000 | Memory Bank by Airglow is licensed under a Attribution License. |
| music-fma-0001 | Memory Bank by Airglow is licensed under a Attribution License. |
| music-fma-0004 | Memory Bank by Airglow is licensed under a Attribution License. |
| music-fma-0005 | Memory Bank by Airglow is licensed under a Attribution License. |
| music-fma-0006 | Alec's band by Alec's Band is licensed under a Attribution License. |
| music-fma-0008 | Alec's band by Alec's Band is licensed under a Attribution License. |
| music-fma-0010 | Stormy Blues by Arne Huseby is licensed under a Attribution 3.0 International License. / Warm Duck Shuffle by Arne Huseby is licensed under a Attribution 3.0 International License. |
| music-fma-0011 | Stormy Blues by Arne Huseby is licensed under a Attribution 3.0 International License. / Warm Duck Shuffle by Arne Huseby is licensed under a Attribution 3.0 International License. |
| music-fma-0013 | Seeing Things by BrokeMC is licensed under a Attribution 3.0 United States License. |
| music-fma-0014 | Seeing Things by BrokeMC is licensed under a Attribution 3.0 United States License. |
| music-fma-0016 | Seeing Things by BrokeMC is licensed under a Attribution 3.0 United States License. |
| music-fma-0018 | Wild Heart by Bryan Mathys is licensed under a Attribution License. |
| music-fma-0019 | Wild Heart by Bryan Mathys is licensed under a Attribution License. |
| music-fma-0020 | Wild Heart by Bryan Mathys is licensed under a Attribution License. |
| music-fma-0030 | Emotion Music by Fabian Measures is licensed under a Attribution License. |
| music-fma-0031 | Emotion Music by Fabian Measures is licensed under a Attribution License. |
| music-fma-0033 | Emotion Music by Fabian Measures is licensed under a Attribution License. |
| music-fma-0034 | baby boys' summer by Factory Sounds is licensed under a Attribution License. |
| music-fma-0036 | baby boys' summer by Factory Sounds is licensed under a Attribution License. |
| music-fma-0038 | baby boys' summer by Factory Sounds is licensed under a Attribution License. |
| music-fma-0039 | baby boys' summer by Factory Sounds is licensed under a Attribution License. |
| music-fma-0040 | baby boys' summer by Factory Sounds is licensed under a Attribution License. |
| music-fma-0043 | Lost Time by Hi-Rez is licensed under a Attribution License. |
| music-fma-0044 | Lost Time by Hi-Rez is licensed under a Attribution License. |
| music-fma-0045 | Lost Time by Hi-Rez is licensed under a Attribution License. |
| music-fma-0046 | Lost Time by Hi-Rez is licensed under a Attribution License. |
| music-fma-0059 | Audionautix: Acoustic by Jason Shaw is licensed under a Attribution 3.0 United States License. / Permissions beyond the scope of this license may be available at http://audionautix.com/index. |
| music-fma-0060 | Audionautix: Acoustic by Jason Shaw is licensed under a Attribution 3.0 United States License. / Permissions beyond the scope of this license may be available at http://audionautix.com/index. |
| music-fma-0061 | Audionautix: Acoustic by Jason Shaw is licensed under a Attribution 3.0 United States License. / Permissions beyond the scope of this license may be available at http://audionautix.com/index. |
| music-fma-0068 | Crawford Street by Josh Woodward is licensed under a Attribution License. / Permissions beyond the scope of this license may be available at http://www.joshwoodward.com/sharing/ or contact artist via email. |
| music-fma-0070 | Crawford Street by Josh Woodward is licensed under a Attribution License. / Permissions beyond the scope of this license may be available at http://www.joshwoodward.com/sharing/ or contact artist via email. |
| music-fma-0071 | Crawford Street by Josh Woodward is licensed under a Attribution License. / Permissions beyond the scope of this license may be available at http://www.joshwoodward.com/sharing/ or contact artist via email. |
| music-fma-0073 | Crawford Street by Josh Woodward is licensed under a Attribution License. / Permissions beyond the scope of this license may be available at http://www.joshwoodward.com/sharing/ or contact artist via email. |
| music-fma-0074 | The Remixes by Kellee Maize is licensed under a Attribution License. |
| music-fma-0076 | The Remixes by Kellee Maize is licensed under a Attribution License. |
| music-fma-0078 | The Remixes by Kellee Maize is licensed under a Attribution License. |
| music-fma-0079 | The Remixes by Kellee Maize is licensed under a Attribution License. |
| music-fma-0080 | The Remixes by Kellee Maize is licensed under a Attribution License. |
| music-fma-0083 | Prelude by Kelly Latimore is licensed under a Attribution License. |
| music-fma-0084 | Prelude by Kelly Latimore is licensed under a Attribution License. |
| music-fma-0085 | Prelude by Kelly Latimore is licensed under a Attribution License. |
| music-fma-0086 | Prelude by Kelly Latimore is licensed under a Attribution License. |
| music-fma-0099 | CC BY 4.0 |
| music-fma-0100 | CC BY 4.0 |
| music-fma-0101 | Pop Singles Compilation 2014 by Rolemusic is licensed under a Attribution License. |
| music-fma-0103 | Pop Singles Compilation 2014 by Rolemusic is licensed under a Attribution License. |
| music-fma-0114 | Give Me a Beat by Tricky Diesel is licensed under a Creative Commons Attribution License. |
| music-fma-0115 | Give Me a Beat by Tricky Diesel is licensed under a Creative Commons Attribution License. |
| music-fma-0116 | Give Me a Beat by Tricky Diesel is licensed under a Creative Commons Attribution License. |
