# N-Bus (Büttner/Dometic NDS) protocol-map

Reverse-engineered uit logic-analyzer captures op de RJ12/6P6C-buskabel van een
Ford Nugget met Büttner/Dometic NDS-systeem (2× Tempra TLB150 accu + MPPT solar charger).

## Fysieke laag

- **Bus**: LIN (single-wire), idle hoog op accuspanning, dominant = naar 0 getrokken.
- **Baudrate**: 19200 baud, 8N1.
- **Connector**: 6P6C (RJ12).
  - pin 1 = +12 V
  - pin 2 = GND
  - pin 3 = LIN data
  - pin 4 = 2e datalijn / wake (meestal inactief)
  - pin 5 = +12 V (rechtstreeks accu, stabielst)
  - pin 6 = NC
- Meetopstelling: data via spanningsdeler (988 Ω + 2×220 Ω → ~3,7 V) naar logic analyzer,
  GND naar pin 2.

## Transportlaag (LIN-TP / diagnostic)

De master polt cyclisch met de diagnostic frames:
- **PID 0x3C** = master request
- **PID 0x3D** (verzonden als 0x7D incl. pariteit) = slave response

Frame-payload (8 databytes): `NAD  PCI  SID  reg  d0 d1 d2 d3` + checksum.

- **NAD** = node-adres (0x81 = solar charger, 0x85 = accu, 0x80 = broadcast)
- **PCI** = 0x06 (single frame, 6 bytes volgen)
- **SID** = 0xB4 (read request) → antwoord 0xF4 (= 0xB4 + 0x40, positieve respons)
- **reg** = parameter-index; node roteert door zijn parameters, 1 per respons
- **d0..d3** = waarde (FF = padding/ongebruikt)

## Nodes

| NAD | Apparaat | Serie |
|-----|----------|-------|
| 0x85 | Leisure battery (Tempra TLB150) | KAA… |
| 0x81 | Solar charger (MPPT) | ACD… |

## Register-map — NAD 0x85 (accu)

| reg | bytes | betekenis | eenheid | status |
|-----|-------|-----------|---------|--------|
| 0x02 | `Vh Vl Ih Il` | accuspanning + accustroom | V=0,01V; I=0,01A, **bit15=1 → ontladen** | BEVESTIGD |
| 0x0B | `b0` | State of Charge | % (0x4B = 75%) | BEVESTIGD |
| 0x56 | `c1h c1l c2h c2l` | celspanningen | 0,001 V (~3,3 V) | WAARSCHIJNLIJK |
| 0x57 | `c3h c3l c4h c4l` | celspanningen | 0,001 V | WAARSCHIJNLIJK |
| 0x54 | ASCII | serienummer-fragment ("KAA") | tekst | BEVESTIGD |

## Register-map — NAD 0x81 (solar charger)

| reg | bytes | betekenis | eenheid | status |
|-----|-------|-----------|---------|--------|
| 0x02 | `Vh Vl Ih Il` | laadspanning + solar-laadstroom | V=0,01V; I=0,01A | BEVESTIGD |
| 0x01 | `Vh Vl` | startaccu-spanning | 0,01 V | BEVESTIGD |
| 0x1B | `Vh Vl` | vermoedelijk paneel-/ingangsspanning (fluctueert) | 0,01 V? | ONZEKER |
| 0x54 | ASCII | serienummer-fragment ("ACD") | tekst | BEVESTIGD |

## Verificatie (capture 4/5/6 vs app)

| Grootheid | cap4 hex → waarde | app4 | cap5 → waarde | app5 | cap6 → waarde | app6 |
|-----------|------|------|------|------|------|------|
| Accu V (85.02) | 0532 → 13,30 | 13,3 | 0530 → 13,28 | 13,3 | 0537 → 13,35 | 13,4 |
| Accu I (85.02) | 8265 → −6,13 | −6,2 | 8473 → −11,39 | −11,4 | 8053 → −0,83 | −1,0 |
| SoC (85.0B) | (74) | 75 | 4B → 75 | 75 | 4B → 75 | 75 |
| Solar I (81.02) | 0012 → 0,18 | 0,2 | 0013 → 0,19 | 0,2 | – | 0,2 |
| Startaccu (81.01) | 04F7 → 12,71 | 12,7 | – | 12,7 | 04F5 → 12,69 | 12,7 |

## Nog te bepalen

- Wh-resterend (cap5 1487 Wh / cap6 1479 Wh) — register nog niet gelokaliseerd.
- Resterende ontlaadtijd.
- Exacte cel-register-indeling (56/57).
- Gedrag bij laden (positieve accustroom): verwacht bit15=0; te bevestigen met dagcapture.
