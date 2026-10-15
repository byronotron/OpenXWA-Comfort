# Multiplayer (LAN and internet)

X-Wing Alliance played multiplayer over DirectPlay. The port replaces DirectPlay with
**NetPlay**, a small UDP transport of its own (`src/xwa_runtime/net/`). The game's own
multiplayer code (the combat chamber lobby, briefing, debrief and the in-flight network
model) is used as recovered; NetPlay only carries its packets.

Status: new and only tested between Linux processes on one machine (see *Testing*).
Expect rough edges until it has been played on real networks.

## Playing

1. Everyone needs the same game files and the same OpenXWA build.
2. Combat Chamber → the multiplayer pod → **Multiplayer**.
3. One player chooses **Host a game**. The screen shows the host's addresses.
4. The others choose **Join a game**. Games on the local network appear in the list
   after a second or two; pick one. To join over the internet (or when the list stays
   empty), type the host's address (`1.2.3.4`, a host name, or `1.2.3.4:port`) and
   choose **Connect to the address**.
5. In the mission setup the host picks the mission, every player drags their name onto
   a craft and presses **Ready**. When everyone is ready and placed, the briefing starts.

Up to 8 players. New players can only join while the host is in the mission setup.

### Network requirements

- UDP port **47630**. Clients talk only to the host, which relays everything, so only the
  host needs to be reachable: for internet games forward UDP 47630 on the host's router
  to the host machine. Clients need nothing.
- LAN discovery uses UDP broadcast. On the Quest the game takes a Wi-Fi multicast lock
  so broadcasts reach it. Some routers (guest networks, "client isolation") block
  broadcasts or traffic between devices; type the address in that case.

## How it fits together

```
 lobby / briefing / debrief            flight
 FrontendNet_*, MissionSetup_*         FlightNet_*, NetSession_*
        │ Net_SendPacketAndFlush               │ NetSession_SendPacket
        │ (whole packets)                      │ (compact DirectPlay packets)
        ▼                                      ▼
   frontend DirectPlay stand-in         flight DirectPlay stand-in
        └──────────── netplay_game.c ──────────┘
                           │
                       netplay.c  (UDP, reliable, ordered, star via the host)
```

- `netplay.c`: sockets on a background thread (acknowledgements and keepalives continue
  while the game thread loads a mission), reliable in-order delivery per sender with
  selective acknowledgements, the host relaying broadcasts and player-to-player
  messages, join/leave/rename events, LAN discovery.
- `netplay_game.c`: the two DirectPlay stand-ins (`Send`, `SetPlayerName`, …), the pump
  that sorts arriving packets into the frontend queue (`g_netDirectPlayRuntimeState`) and
  the flight queue (`g_netSessionRecvQueue`), and the flight session setup.
- `netplay_screens.c`: the multiplayer and join screens.
- `frontend_net.c`: `FrontendNet_ProcessNetworkPackets`, reconstructed from the packet
  senders and the callers (the packet types are listed above the function).

### Details worth knowing

- **Broadcasts reach the sender too** on the frontend layer, as the game expects (the
  host leaves for the briefing on its own `'J'`). The host ignores its own state packets.
- **Player ids**: the host is `0x10`, clients count up from `0x11`. The flight session
  orders players by id, so every machine gives every player the same slot and the host
  is slot 0, as the flight code assumes.
- **Flight numbers**: flight packets carry the number of the mission they belong to.
  Packets of a mission this machine has already left are dropped; packets of one it has
  not started yet (a slower machine still loading) wait for it.
- **Sequence numbers**: NetPlay already delivers in order, so arriving flight packets
  are renumbered consecutively per sender before the session's reorder logic sees them.
- **Determinism**: each machine runs the flight itself from the inputs the host
  distributes; the game compares checksums and resends the world when they differ. The
  recovered code is built with `-ffp-contract=off` so ARM (Quest) and x86 compute the
  same floating point results. Mixed Quest/PC games are the least tested case.

### Blocking waits

The recovered flight start (`FlightNet_SyncPlayerOptionsAndTaunts`,
`FlightNet_WaitForMissionStart`) waits in loops until every player has loaded, up to
60 seconds. The headset shows its loading indicator meanwhile.

## Testing

`XWA_NETPLAY_LOSS=<percent>` drops that share of datagrams in both directions, to test
the reliability layer.

The development tests (not in the build) ran:

- the transport alone: a host and 2–4 client processes over loopback exchanging 3000
  frontend and 3000 flight messages each, in order and intact, at 0 %, 20 % and 40 %
  loss;
- the game layers linked from `libxwa_core`: a host and two clients joining, the lobby
  state (`'A'`, `'t'`, `111`), a client's craft slot edit (`'L'`), ready flags, the
  briefing start with its random seed, the flight session (slots, the 20/19/27 start
  handshake, 300 world messages and 300 inputs per client in order), the return to the
  mission setup, a player leaving and the host quitting, at 0 %, 15 % and 30 % loss.

Not yet tested: the screens themselves and a real mission, on any platform.

## Not done

- No chat panel (`FrontendNet_UpdateAndDrawPanel` is still a stub).
- No joining a game in progress, no passwords, no IPv6, no NAT traversal (the host
  forwards a port).
- The original "sequenced resend" packets are understood on arrival but nothing sends
  them; NetPlay does the resending.
