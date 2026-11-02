**Pre-release / playtest build. Online play is still in testing.**

JStarsReborn 0.5.1 is the 0.4.0 Windows port plus the first online work. Offline play is unchanged.

### Online (experimental, off by default) — peer-to-peer, no server
- Turn it on in the launcher's new **Online** page. The two players' games talk to each other directly; there is no JStarsReborn server, nothing contacts PlayStation Network, and no account is used.
- **Same network:** rooms are found automatically.
- **Over the internet:** the host presses **Get my room code** (the launcher asks the router to open UDP ports 47391 and 3658 with UPnP, and closes them again on exit) and sends the code to the other player, who types it into **Friend's room code**. Without UPnP, forward UDP 47391 and 3658 to the host PC by hand and use the host's public address instead.
- In the game: Online Battle > Friendly Match. The host chooses **Create a Room**, the other player **Search for a Room**.
- **What works:** hosting a room; seeing another player's room.
- **What does NOT work yet:** **joining a room**, so a match between two players cannot start yet; Rank Match; Quick Match. Unfinished functions make the game show "A communication error has occurred" and return to the menu. UPnP has been tested against a simulated router only.

### Requirements
Windows 10/11 x64, a Vulkan 1.3 GPU driver, and **your own legally obtained, decrypted dump** of J-Stars Victory VS+ CUSA01593 v01.02. No game files, keys or firmware are included. See DISCLAIMER.txt. Piracy is not supported.

Not affiliated with or endorsed by the game's publisher, developer or rights holders. Provided as is, without warranty.
