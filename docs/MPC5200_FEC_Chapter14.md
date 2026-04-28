# Chapter 14: Fast Ethernet Controller (FEC)

_Sliced from MPC5200_Users_Guide.md (PDF pages 455-500, slide markers preserved)._

<!-- Slide number: 455 -->
# Overview

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-1
Chapter 14 
Fast Ethernet Controller (FEC)
14.1
Overview
The fast Ethernet controller (FEC) is an ethernet MAC plus two 1 Kbyte FIFOs that work under the control of the processor and BestComm 
DMA engine to support 10/100 Mbps Ethernet/802.3 networks. Table 14-1 shows a block diagram.
A brief introduction and overview of the major functional blocks aid in understanding and programming the FEC.
The FEC is controlled by writing through the system interface (SIF) module into control registers located in each block. The control/status 
register (CSR) block provides global control and interrupt handling registers. User programming of the CSR is the primary focus of this 
chapter.
The RISC based controller provides the following functions:
•
Initialization
•
Address recognition for receive frames
•
Random number generation for transmit collision backoff timer
The FIFO controller is the focal point of all data flow in the FEC. The FIFO is divided into a transmit and receive FIFO of 1Kbyte each. 
Transmit data flows from the CommBus into the transmit FIFO and through the transmit block to the physical layer device (PHY). Receive 
data flows from the PHY to the receive block and is pulled out of the FIFO by BestComm. BestComm data transfers are interrupt driven. 
Interrupt driven data movement from the processor is not supported.
The bus controller decides which block is to be the T-bus master for each cycle. All the blocks receive their control information over the T-bus 
and, for the most part, provide status information over this same internal bus.
The media independent interface (MII) block provides a serial channel for control/status communication with the external physical layer 
device (transceiver or PHY). The serial channel consists of the MDC (clock) and MDIO (bidirectional data I/O) lines of the MII interface.
The transmit and receive blocks provide the ethernet MAC functionality (with some assistance from the microcode). Internal to these blocks 
are clock domain boundaries between the system clock and the network clocks supplied by the PHY.
The management information base (MIB) block maintains the counters for a variety of network events and statistics. The counters support the 
RMON (RFC 1757) ethernet statistics group and some of the IEEE 802.3 counters.
The FEC supports several standard MAC-PHY interfaces to connect to an external ethernet transceiver. One is the 10/100 Mbps MII interface. 
Another is the 10-Mbps only 7-Wire interface, which uses a subset of the MII pins.

<!-- Slide number: 456 -->
# MPC5200 Users Guide, Rev. 3.1

14-2
Freescale Semiconductor
Overview
Figure 14-1. Block Diagram—FEC
14.1.1
Features
The FEC incorporates several features/design goals that are key to its use:
•
Support for different ethernet physical interfaces:
—
100 Mbps IEEE 802.3 MII
—
10 Mbps IEEE 802.3 MII
—
10 Mbps 7-wire interface (industry standard)
•
IEEE 802.3 full-duplex flow control
•
Programmable max frame length supports IEEE 802.1 VLAN tags and priority
•
Support for full-duplex operation (200 Mbps throughput) with a minimum system clock rate of 50 MHz.
•
Support for half-duplex operation (100 Mbps throughput) with a minimum system clock rate of 25 MHz.
•
Large (1 Kbyte) on-chip transmit and receive FIFOs to support a variety of bus latencies.
•
Retransmission from transmit FIFO following a collision (no processor bus utilization).
CSR
FIFO Controller
RISC
Controller
MII
Receive
Transmit
tbus
requests
tbuss_addr
tbusd_addr
tbus_addr
MDC
MDIO
RX_CLK
RX_DV
RXD[3:0]
RX_ER
TX_CLK
TX_EN
TXD[3:0]
TX_ER
CRS,COL
MIB
(RISC +
microcode)
I/O
Pad
MDO
MDEN
FEC
Counters
MII/7-wire Data
Option
MDI
SIF
Bus
Controller
IP bus
T-bus
Tx FIFO (1KByte)
CLK/CNTL
CommBus
Interrupt
Rx FIFO (1KByte)

<!-- Slide number: 457 -->
# Modes of Operation

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-3
•
Automatic internal flushing of the Rx FIFO for runts (collision fragments) and address recognition rejects (no processor bus 
utilization).
•
Address recognition
—
Frames with broadcast address may be always accepted or always rejected
—
Exact match for single 48-bit individual (unicast) address
—
Hash (64-bit hash) check of individual (unicast) addresses
—
Hash (64-bit hash) check of group (multicast) addresses
—
Promiscuous mode
14.2
Modes of Operation
The primary operational modes are described in this section.
14.2.1
Full- and Half-Duplex Operation
This is determined by the X_CNTRL register FDEN bit. Full-duplex mode is intended for use on point to point links between switches or end 
node to switch. Half-duplex mode is used in connections between an end node and a repeater or between repeaters.
Full-duplex flow control is an option that may be enabled in full-duplex mode.
14.2.2
10Mbps and 100Mbps MII Interface Operation
The MAC-PHY interface operates in MII mode by asserting the R_CNTRL register MII_MODE bit. MII is the media independent interface 
defined by the 802.3 standard for 10/100 Mbps operation.
Speed of operation is determined by the TX_CLK and RX_CLK pins, which are driven by the transceiver. The transceiver either 
auto-negotiates the speed or it may be controlled by software using the serial management interface (MDC/MDIO pins) to the transceiver.
14.2.3
10Mbps 7-Wire Interface Operation
If the external transceiver supports 10 Mbps only and uses a 7-wire style interface then deassert the R_CNTRL register MII_MODE bit in the 
R_CNTRL register. This style of interface is not defined by the 802.3 standard, but instead is an industry standard.
14.2.4
Address Recognition Options
The options supported are promiscuous, broadcast reject, individual address hash or exact match and multicast hash match. Refer to the 
R_CNTRL register for address recognition programming.
14.2.5
Internal Loopback
Internal loopback mode is selected using the R_CNTRL register LOOP bit. 
14.3
I/O Signal Overview
This section defines the FEC-to-chip pin I/O. The FEC network interface supports multiple options. One is the MII option that requires 18 
I/O pins and supports both data and an out-of-band serial management interface to the PHY (transceiver) device. The MII option supports 
both 10 and 100 Mbps ethernet rates. The second is referred to as the 7-wire interface and supports only 10 Mbps ethernet data. The 7-wire 
interface uses a subset of the MII signals.
Table 14-1 shows the network interface signals and lists 18 signals, all of which are used for the 10/100 MII interface.
NOTE
The MDIO pin is bidirectional and corresponds to the FEC block MDI, MDO and MDIO pins. The 
7-wire interface option uses a subset of these signals.
Table 14-1. Signal Properties
Signal Name
Chip Pin
Function
Reset State
tx_en
ETH0
MII—transmit data valid output
7-wire—transmit data valid output
0
tdata[0]
ETH1
MII—transmit data bit 0 output
7-wire—transmit data output

<!-- Slide number: 458 -->
# MPC5200 Users Guide, Rev. 3.1

14-4
Freescale Semiconductor
I/O Signal Overview
14.3.1
Detailed Signal Descriptions
14.3.1.1
MII Ethernet MAC-PHY Interface
This section gives a detailed description of the Media-Independent Interface (MII). An overview of the MII is presented followed by a 
description of the MII signals. Two different types of MII frames are described. A brief MII management function overview is given.
The MII interface has 18 signals. Tx and Rx functions require 7 signals each:
•
4 data signals
•
1 delimiter
•
1 error
•
1 clock
Media status is indicated by 2 signals:
•
1 signal indicates a carrier is present.
•
1 signal indicates a collision occurred.
Management interface is provided by 2 signals.
MII signals are described below.
Tx_CLK. . . . . . . . . . . . A continuous clock that provides a timing reference for Tx_EN, TxD, and Tx_ER. The frequency of Tx_CLK 
is 25% of the transmit data rate, ± 100 ppm. Duty cycle shall be 35%-65% inclusive.
Rx_CLK. . . . . . . . . . . . A continuous clock that provides a timing reference for Rx_DV, RxD, and Rx_ER. The frequency of Rx_CLK 
is 25% of the Rx data rate, with a duty cycle between 35% and 65%.
tdata[1]
ETH2
MII—transmit data bit 1 output
tdata[2]
ETH3
MII—transmit data bit 2 output
tdata[3]
ETH4
MII—transmit data bit 3 output
tx_er
ETH5
MII—transmit error output
0
mdc
ETH6
MII—management clock output
0
mdi
mdo
md_en
ETH7
MII—management data bidirect
Hi-Z (input)
rx_dv
ETH8
MII—Rx data valid input
7-wire—rena input
rx_clk
ETH9
MII—Rx clock input
7-wire—Rx clock input
col
ETH10
MII—collision input
10 Mbps 7-wire—collision input
tx_clk
ETH11
MII—transmit clock input
7-wire—transmit clock input
rdata[0]
ETH12
MII—Rx data bit 0 input
7-wire—Rx data input
rdata[1]
ETH13
MII—Rx data bit 1 input
rdata[2]
ETH14
MII—Rx data bit 2 input
rdata[3 ]
ETH15
MII—Rx data bit 3 input
rx_er
ETH16
MII—Rx error input
crs
ETH17
MII—carrier sense input
Table 14-1. Signal Properties (continued)
Signal Name
Chip Pin
Function
Reset State

<!-- Slide number: 459 -->
# I/O Signal Overview

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-5
Tx_EN  . . . . . . . . . . . . . Assertion of this signals indicates valid nibbles are being presented on the MII. This signal is asserted with the 
first nibble of preamble and is negated prior to the first Tx_CLK following the final nibble of the frame.
TxD. . . . . . . . . . . . . . . . TxD[0:3] represent a nibble of data when Tx_EN is asserted and have no meaning when Tx_EN is de-asserted. 
Table 14-2 summarizes the permissible encoding of TxD.
Tx_ER  . . . . . . . . . . . . . Assertion of this signal for one or more clock cycles while Tx_EN is asserted causes PHY to transmit one or 
more illegal symbols. Asserting Tx_ER has no affect when operating at 10 Mbps or when Tx_EN is de-asserted 
This signal transitions synchronously with respect to Tx_CLK.
Rx_DV . . . . . . . . . . . . . When this signal is asserted, PHY is indicating a valid nibble is present on the MII. This signal remains asserted 
from the first recovered nibble of the frame through the last nibble. Assertion of Rx_DV must start no later than 
the SFD, and exclude any EOF.
RxD. . . . . . . . . . . . . . . . RxD[0:3] represents a nibble of data to be transferred from the PHY to the MAC when Rx_DV is asserted. A 
completely formed SFD must be passed across the MII. When Rx_DV is not asserted, RxD has no meaning. 
There is an exception to this which is explained later. Table 14-3 summarizes the permissible encoding of RXD. 
Rx_ER . . . . . . . . . . . . . When Rx_ER and Rx_DV are asserted, the PHY has detected an error in the current frame.
When Rx_DV is not asserted, Rx_ER shall have no affect. This signal transitions synchronously with Rx_CLK 
CRS  . . . . . . . . . . . . . . . Signal is asserted when Tx or Rx medium is not idle. If a collision occurs, CRS remains asserted through the 
duration of the collision. This signal is not required to transition synchronously with Tx_CLK or Rx_CLK.
COL . . . . . . . . . . . . . . . Signal is asserted on a collision detection, and remains asserted while the collision persists. The signal behavior 
is not specified when in full-duplex mode. This signal is not required to transition synchronously with Tx_CLK 
or Rx_CLK.
MDC. . . . . . . . . . . . . . . Signal provides a timing reference to the PHY for data transfers on the MDIO signal. MDC is aperiodic, and 
has no maximum high or low times. The minimum high and low times is 160 ns, with the minimum period being 
400 ns.
MDIO . . . . . . . . . . . . . . Signal transfers control/status information between the PHY and MAC. It transitions synchronously to MDC. 
The MDIO pin is a bidirectional pin. The internal FEC signals that connect to this pad are: MDI (data in), MDO 
(data out), and MD_EN (direction control, high for output).
Table 14-2 lists the interpretation of possible encodings for Tx_EN and Tx_ER
A false carrier condition occurs if PHY detects a bad start-of-stream delimiter. This condition signals MII by asserting Rx_ER and placing 
1110 on RxD. Rx_DV must also be de-asserted. Valid Rx_DV, Rx_ER and RxD[3:0] encodings are shown in Table 14-3.
14.3.1.2
MII Management Frame Structure
A transceiver management frame transmitted on the MII management interface uses the MDIO and MDC pins. A transaction or frame on this 
serial interface has the following format:
Table 14-2. MII: Valid Encoding of TxD, Tx_EN and Tx_ER
TX_EN
TX_ER
TXD
Indication
0
0
0000 through 1111
Normal inter-frame
0
1
0000 through 1111
Reserved
1
0
0000 through 1111
Normal data transmission
1
1
0000 through 1111
Transmit error propagation
Table 14-3. MII: Valid Encoding of RxD, Rx_ER and Rx_DV
RX_DV
RX_ER
RXD
Indication
0
0
0000 through 1111
Normal inter-frame
0
1
0000
Normal inter-frame
0
1
0001 through 1101
Reserved
0
1
1110
False Carrier
0
1
1111
Reserved
1
0
0000 through 1111
Normal data reception
1
1
0000 through 1111
Data reception with errors

<!-- Slide number: 460 -->
# MPC5200 Users Guide, Rev. 3.1

14-6
Freescale Semiconductor
FEC Memory Map and Registers
<preamble><st><op><phyad><regad><ta><data><idle>
14.3.1.2.1
MII Management Register Set
The MII management register set located in the PHY may consist of a basic register set and an extended register set as defined in Table 14-5.
14.4
FEC Memory Map and Registers
The FEC device is programmed by a combination of control/status registers (CSRs) and BestComm task loops. Since the FEC software model 
is BestComm-based, there is no similarity with existing CPM-based products’ coding. 
The CSRs are used for mode control, interrupts and extraction of status information. BestComm tasks are used to pass data buffers and related 
buffer or frame information between the hardware and software.
All access via microprocessor to and from the registers must be 32-bit accesses. There is no support for accesses other than 32-bit. All access 
via BestComm to and from the registers may be byte, word or longword (32-bit) accesses. However, name based register access is 32-bit 
aligned.
Table 14-4. MMI Format Definitions
Name
Description
<preamble>
Optional—consists of a sequence of 32 continuous logic 1s.
<st>
Start of frame—indicated by a <01> pattern.
<op>
Operation code:
Read instruction is <10>
Write instruction is <01>
<phyad>
A 5-bit field that lists up to 32 PHYs be addressed. The first address bit transmitted is the 
msb of the address.
<regad>
A 5-bit field that lets 32 registers be addressed within each PHY. The first register bit 
transmitted is the msb of the address.
<ta>
A 2-bit field that provides spacing between the register address field and the data field to 
avoid contention on the MDIO signal during a read operation.
<data>
Data field is 16 bits wide. Data bit 15 is first bit transmitted and received.
<idle>
During idle condition, MDIO is in the high impedance state.
Table 14-5. MII Management Register Set
Register Address
Register Name
Basic/Extended
0
Control
B
1
Status
B
2:3
PHY Identifier
E
4
Auto-Negotiation Advertisement
E
5
AN Link Partner Ability
E
6
AN Expansion
E
7
AN Next Page Transmit
E
8:15
Reserved
E
16:31
Vendor Specific
E

<!-- Slide number: 461 -->
# FEC Memory Map and Registers

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-7
14.4.1
Top Level Module Memory Map
The FEC implementation requires a 2KByte memory map space. This is divided into two sections of 512 Bytes and an additional 1KBytes 
of reserved space. The first 512 Bytes is used for Control and Status Registers. The second contains event/statistic counters held in the MIB 
block. Table 14-6 defines the top level memory map.
14.4.2
Control and Status (CSR) Memory Map
Table 14-6. Module Memory Map
Address
Function
000–1FF
Control/Status Registers
200–3FF
MIB Block Counters, see Table 14-7
400–7FF
Reserved
TABLE 1. CSR Counters
Address
Mnemonic
Name
000
FEC_ID
FEC_ID register
004
IEVENT
Interrupt Event Register
008
IMASK
Interrupt Enable Register
00C
Reserved
010
R_DES_ACTIVE
Receive Ring Updated Flag
014
X_DES_ACTIVE
Transmit Ring Updated Flag
018-020
Reserved
024
ECNTRL
Ethernet Control Register
028-03C
Reserved
040
MII_DATA
MII Data Register
044
MII_SPEED
MII Speed Register
04C-060
Reserved 
064
MIB_CONTROL
MIB Control/Status Register
068-080
Reserved
084
R_CNTRL
Receive Control register
088
R_HASH
Receive Hash
08C-0C0
Reserved
0C4
X_CNTRL
Transmit Control Register
0C8-0E0
Reserved
0E4
PADDR1
Physical Address Low
0E8
PADDR2
Physical Address High+ Type Field
0EC
OP_PAUSE
Opcode + Pause Duration
0F0-114
Reserved
118
IADDR1
Upper 32 bits of individual Hash Table
11C
IADDR2
Lower 32 bits of individual Hash Table

<!-- Slide number: 462 -->
# MPC5200 Users Guide, Rev. 3.1

14-8
Freescale Semiconductor
FEC Memory Map and Registers
14.4.3
MIB Block Counters Memory Map
Table 14-7 defines the MIB Counters memory map, which defines the MIB RAM space locations where hardware-maintained counters reside. 
These fall in the 3200-33FF address range. Counters are divided into two groups.
1.
RMON counters—are included, which cover ethernet statistics counters defined in RFC 1757. In addition to ethernet statistics 
group counters, a counter is included to count truncated frames, as FEC only supports frame lengths up to 2047Bytes. RMON 
counters are implemented independently for Tx and Rx, to ensure accurate network statistics when operating in full duplex mode.
2.
IEEE counters—are included, which support the Mandatory and Recommended counter packages defined in Section 5 of 
ANSI/IEEE Standard 802.3 (1998 edition). FEC supports IEEE Basic Package objects, but does not require MIB block counters. 
In addition, some recommended package objects supported do not require MIB counters. Counters for Tx and Rx full duplex flow 
control frames are included.
120
GADDR1
Upper 32 bits of group Hash Table
124
GADDR2
Lower 32 bits of group Hash Table
128-140
Reserved
144
X_WMRK
Transmit FIFO Watermark
148-180
Reserved
184
RFIFO_DATA
Receive FIFO Data
188
RFIFO_STATUS
Receive FIFO Status
18C
RFIFO_CONTROL
Receive FIFO Control
190
RFIFO_LRF_PTR
Receive FIFO Last Read Frame Pointer
194
RFIFO_LWF_PTR
Receive FIFO Last Write Frame Pointer
198
RFIFO_ALARM
Receive FIFO Alarm Pointer
19C
RFIFO_RDPTR
Receive FIFO Read Pointer
1A0
RFIFO_WRPTR
Receive FIFO Write Pointer
1A4
TFIFO_DATA
Transmit FIFO Data
1A8
TFIFO_STATUS
Transmit FIFO Status
1AC
TFIFO_CONTROL
Transmit FIFO Control
1B0
TFIFO_LRF_PTR
Transmit FIFO Last Read Frame Pointer
1B4
TFIFO_LWF_PTR
Transmit FIFO Last Write Frame Pointer
1B8
TFIFO_ALARM
Transmit FIFO Alarm Pointer
1BC
TFIFO_RDPTR
Transmit FIFO Read Pointer
1C0
TFIFO_WRPTR
Transmit FIFO Write Pointer
1C4
RESET_CNTRL
Reset Control
1C8
XMIT_FSM
Transmit FSM
1CC-1FF
TABLE 1. CSR Counters
Address
Mnemonic
Name

<!-- Slide number: 463 -->
# FEC Memory Map and Registers

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-9
Table 14-7. MIB Counters
Address
Mnemonic
Description
200
RMON_T_DROP
Count of Frames Not Correctly Counted
204
RMON_T_PACKETS
RMON Tx Packet Count
208
RMON_T_BC_PKT
RMON Tx Broadcast Packets
20C
RMON_T_MC_PKT
RMON Tx Multicast Packets
210
RMON_T_CRC_ALIGN
RMON Tx Packets with CRC/Align error
214
RMON_T_UNDERSIZE
RMON Tx Packets less than 64bytes, good CRC
218
RMON_T_OVERSIZE
RMON Tx Packets greater than MAX_FL bytes, good CRC
21C
RMON_T_FRAG
RMON Tx Packets less than 64bytes, bad CRC
220
RMON_T_JAB
RMONTxPackets greater than MAX_FL bytes, bad CRC
224
RMON_T_COL
RMON Tx collision count
228
RMON_T_P64
RMON Tx 64Byte packets
22C
RMON_T_P65TO127
RMON Tx 65 to 127Byte packets
230
RMON_T_P128TO255
RMON Tx 128 to 255Byte packets
234
RMON_T_P256TO511
RMON Tx 256 to 511Byte packets
238
RMON_T_P512TO1023
RMON Tx 512 to 1023Byte packets
23C
RMON_T_P1024TO2047
RMON Tx 1024 to 2047Byte packets
240
RMON_T_P_GTE2048
RMON Tx packets with greater than 2048Bytes
244
RMON_T_OCTETS
RMON Tx Octets
248
IEEE_T_DROP
Count of Frames Not Counted Correctly
24C
IEEE_T_FRAME_OK
Frames Transmitted OK
250
IEEE_T_1COL
Frames Transmitted with Single Collision
254
IEEE_T_MCOL
Frames Transmitted with Multiple Collisions
258
IEEE_T_DEF
Frames Transmitted after Deferral Delay
25c
IEEE_T_LCOL
Frames Transmitted with Late Collision
260
IEEE_T_EXCOL
Frames Transmitted with Excessive Collisions
264
IEEE_T_MACERR
Frames Transmitted with Tx FIFO Underrun
268
IEEE_T_CSERR
Frames Transmitted with Carrier Sense Error
26C
IEEE_T_SQE
Frames Transmitted with SQE Error
270
T_FDXFC
Flow Control Pause Frames Transmitted
274
IEEE_T_OCTETS_OK
Octet Count for Frames Transmitted w/o Error
278–27C
rsvd
Reserved
280
RMON_R_DROP
Count of frames Not Counted Correctly
284
RMON_R_PACKETS
RMON Rx Packet Count
288
RMON_R_BC_PKT
RMON Rx Broadcast Packets
28C
RMON_R_MC_PKT
RMON Rx Multicast Packets

<!-- Slide number: 464 -->
# MPC5200 Users Guide, Rev. 3.1

14-10
Freescale Semiconductor
FEC Registers—MBAR + 0x3000
14.5
FEC Registers—MBAR + 0x3000
The FEC uses 37 32-bit registers. These registers are located at an offset from MBAR of 0x3000. Register addresses are relative to this offset. 
Therefore, the actual register address is MBAR + 0x3000 + register address
Hyperlinks to the FEC registers are provided below:
290
RMON_R_CRC_ALIGN
RMON Rx Packets with CRC/Align error
294
RMON_R_UNDERSIZE
RMON Rx Packets less than 64Bytes, good CRC
298
RMON_R_OVERSIZE
RMON Rx Packets greater than MAX_FL bytes, good CRC
29C
RMON_R_FRAG
RMON Rx Packets less than 64Bytes, bad CRC
2A0
RMON_R_JAB
RMONRxPackets greater than MAX_FL bytes, bad CRC
2A4
RMON_R_RESVD_0
Reserved
2A8
RMON_R_P64
RMON Rx 64Byte packets
2AC
RMON_R_P65TO127
RMON Rx 65 to 127Byte packets
2B0
RMON_R_P128TO255
RMON Rx 128 to 255Byte packets
2B4
RMON_R_P256TO511
RMON Rx 256 to 511Byte packets
2B8
RMON_R_P512TO1023
RMON Rx 512 to 1023Byte packets
2BC
RMON_R_P1024TO2047
RMON Rx 1024 to 2047Byte packets
2C0
RMON_R_P_GTE2048
RMON Rx packets with greater than 2048Bytes
2C4
RMON_R_OCTETS
RMON Rx Octets
2C8
IEEE_R_DROP
Count of frames not counted correctly
2CC
IEEE_R_FRAME_OK
Frames received OK
2D0
IEEE_R_CRC
Frames received with CRC error
2D4
IEEE_R_ALIGN
Frames received with alignment error
2D8
IEEE_R_MACERR
Rx FIFO overflow count
2DC
R_FDXFC
Flow Control Pause frames received
2E0
IEEE_R_OCTETS_OK
Octet count for frames received without error
2E4–2FC
rsvd
Reserved
300–3FF
rsvd
Reserved
Table 14-7. MIB Counters (continued)
Address
Mnemonic
Description

<!-- Slide number: 465 -->
# FEC Registers—MBAR + 0x3000

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-11
14.5.1
FEC ID Register—MBAR + 0x3000
The read-only FEC ID register (FEC_ID) identifies the FEC block and revision.
 
•
Section 14-8, FEC ID Register (0x3000)
•
Section 14-9, FEC Interrupt Event Register (0x3004)
•
Section 14-10, FEC Interrupt Enable Register 
(0x3008)
•
Section 14-11, FEC Rx Descriptor Active Register 
(0x3010)
•
Section 14-12, FEC Tx Descriptor Active Register 
(0x3014)
•
Section 14-13, FEC Ethernet Control Register 
(0x3024)
•
Section 14-14, FEC MII Management Frame Register 
(0x3040)
•
Section 14-15, FEC MII Speed Control Register 
(0x3044)
•
Section 14-17, FEC MIB Control Register (0x3064)
•
Section 14-18, FEC Receive Control Register 
(0x3084)
•
Section 14-19, FEC Hash Register (0x3088)
•
Section 14-20, FEC Tx Control Register (0x30C4)
•
Section 14-21, FEC Physical Address Low Register 
(0x30E4)
•
Section 14-22, FEC Physical Address High Register 
(0x30E8)
•
Section 14-23, FEC Opcode/Pause Duration 
Register (0x30EC)
•
Section 14-24, FEC Descriptor Individual Address 1 
Register (0x3118)
•
Section 14-25, FEC Descriptor Individual Address 2 
Register (0x311C)
•
Section 14-26, FEC Descriptor Group Address 1 
Register (0x3120)
•
Section 14-27, FEC Descriptor Group Address 2 
Register (0x3124)
•
Section 14-28, FEC Tx FIFO Watermark Register 
(0x3144)
•
Section 14.7, FEC Tx FIFO Data Register—MBAR + 
0x31A4 (0x3184)
•
Section 14.6.1, FEC Rx FIFO Data Register—MBAR 
+ 0x3184 (0x31A4)
•
Section 14-30, FEC Rx FIFO Status Register 
(0x3188)
•
Section , FEC Tx FIFO Status Register (0x31A8)
•
Section 14-31, FEC Rx FIFO Control Register 
(0x318C)
•
Section , FEC Tx FIFO Control Register (0x31AC)
•
Section 14-32, FEC Rx FIFO Last Read Frame 
Pointer Register (0x3190)
•
Section , FEC Tx FIFO Last Read Frame Pointer 
Register (0x31B0)
•
Section 14-33, FEC Rx FIFO Last Write Frame 
Pointer Register (0x3194)
•
Section , FEC Tx FIFO Last Write Frame Pointer 
Register (0x31B4)
•
Section 14-34, FEC Rx FIFO Alarm Pointer Register 
(0x3198)
•
Section , FEC Tx FIFO Alarm Pointer Register 
(0x31B8)
•
Section 14-35, FEC Rx FIFO Read Pointer Register 
(0x319C)
•
Section , FEC Tx FIFO Read Pointer Register 
(0x31BC)
•
Section 14-36, FEC Rx FIFO Write Pointer Register 
(0x31A0)
•
Section , FEC Tx FIFO Write Pointer Register 
(0x31C0)
•
Section 14-37, FEC Reset Control Register (0x31C4)
•
Section 14-38, FEC Transmit FSM Register (0x31C8)
Table 14-8. FEC ID Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
FEC_ID
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
DMA
FIFO
Rsvd
FEC_REV
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0

<!-- Slide number: 466 -->
# MPC5200 Users Guide, Rev. 3.1

14-12
Freescale Semiconductor
FEC Registers—MBAR + 0x3000
14.5.2
FEC Interrupt Event Register—MBAR + 0x3004
When an event occurs that sets a bit in the IEVENT register, an interrupt is generated if the corresponding bit in the interrupt enable register 
(IMASK) is also set. The IEVENT register bit is cleared if 1 is written to that bit position. A 0 write has no effect. A hardware reset clears 
this register.
These interrupts can be divided into operational interrupts, transceiver/network error interrupts, and internal error interrupts. Interrupts that 
may occur in normal operation are:
•
GRA
•
TFINT
•
MII
Interrupts resulting from errors/problems detected in the network or transceiver are:
•
HBERR
•
BABR
•
BABT
•
LATE_COL
•
COL_RETRY_LIM
Interrupts resulting from internal errors are:
•
XFIFO_UN
•
XFIFO_ERROR
•
RFIFO_ERROR
Some error interrupts are independently counted in the MIB block counters. Software may choose to mask these interrupts, since the errors 
are visible to network management via the MIB counters.
•
HBERR – IEEE_T_SQE
•
BABR – RMON_R_OVERSIZE (good CRC), RMON_R_JAB (bad CRC)
•
BABT – RMON_T_OVERSIZE (good CRC), RMON_T_JAB (bad CRC)
•
LATE_COL – IEEE_T_LCOL
•
COL_RETRY_LIM – IEEE_T_EXCOL
•
XFIFO_UN – IEEE_T_MACERR
 
Bits
Name
Description
0:15
FEC_ID
Value identifying the FEC
000 = Unique identifier for FEC
16:20
—
Reserved
21
DMA
DMA function is included in the FEC
0 = FEC does not include DMA (BestComm is the DMA engine)
22
FIFO
FIFO function included in the FEC
1 = FEC does include a FIFO
24:31
FEC_REV
Value identifies the FEC revision
00 = Initial revision
Table 14-9. FEC Interrupt Event Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
HBERR
BABR
BABT
GRA
TFINT
Reserved
MII
Rsvd
LATE_COL
COL_
RETRY_LIM
XFIFO_UN
XFIFO_
ERROR
RFIFO_
ERROR
Rsvd
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0

<!-- Slide number: 467 -->
# FEC Registers—MBAR + 0x3000

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-13
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0
HBERR
Heartbeat Error— interrupt bit indicates HBC is set in the X_CNTRL register and COL 
input was not asserted within the heartbeat window following a transmission.
1
BABR
Babbling Receive Error—bit indicates frame was received with a length in excess of 
R_CNTRL.MAX_FL bytes.
2
BABT
Babbling Transmit Error—bit indicates transmitted frame length exceeded 
R_CNTRL.MAX_FL bytes. This condition is usually caused by a frame that is too long 
being placed into the transmit data buffer(s).
Truncation does not occur.
3
GRA
Graceful Stop Complete—interrupt bit is asserted for one of three reasons.
1 = A graceful stop initiated by setting X_CNTRL.GTS bit is complete.
2 = A graceful stop initiated by setting X_CNTRL.FC_PAUSE bit is complete.
3 = A graceful stop initiated by reception of a valid full duplex flow control “pause” 
frame is complete. Refer to “Full Duplex Flow Control” section of the Ethernet 
Operation chapter.
A "graceful stop" means the transmitter is put into a pause state after completion of 
the frame currently being transmitted.
4
TFINT
Transmit frame interrupt. This bit indicates that a frame has been transmitted.
5
—
Reserved
6
—
Reserved
7
—
Reserved
8
MII
MII Interrupt—bit indicates MII completed the data transfer requested.
9
—
Reserved
10
LATE_COL
Bit indicates a collision occurred beyond the collision window (slot time) in half-duplex 
mode. The frame is truncated with a bad CRC. Remainder of the frame is discarded.
11
COL_RETRY_LIM
Collision Retry Limit—bit indicates a collision occurred on each of 16 successive 
attempts to transmit the frame. The frame is discarded without being transmitted and 
transmission of the next frame begins. 
Only occurs in half-duplex mode.
12
XFIFO_UN
Transmit FIFO Underrun—bit indicates the transmit FIFO became empty before the 
complete frame was transmitted. A bad CRC is appended to the frame fragment and 
remainder of frame is discarded.
13
XFIFO_ERROR
Transmit FIFO Error—indicates an error occurred within the forest green version 
transmit FIFO. When XFIFO_ERROR bit is set, ECNTRL.ETHER_EN is cleared, 
halting FEC frame processing. When this occurs, software must ensure both the FIFO 
Controller and BestComm are soft-reset.
14
RFIFO_ERROR
Receive FIFO Error—indicates error occurred within the forest green version RX 
FIFO. When RFIFO_ERROR bit is set, ECNTRL.ETHER_EN is cleared, halting FEC 
frame processing. When this occurs, software must ensure both the FIFO Controller 
and BestComm are soft-reset.
15:31
—
Reserved.

<!-- Slide number: 468 -->
# MPC5200 Users Guide, Rev. 3.1

14-14
Freescale Semiconductor
FEC Registers—MBAR + 0x3000
14.5.3
FEC Interrupt Enable Register—MBAR + 0x3008
The IMASK register provides control over the interrupt events allowed to generate an interrupt. All implemented bits in this CSR are R/W. 
This register is cleared by a hardware reset. If corresponding bits in both the IEVENT and IMASK registers are set, the interrupt is signalled 
to the CPU. The interrupt signal remains asserted until 1 is written to the IEVENT bit (write 1 to clear) or a 0 is written to the IMASK bit.
 
14.5.4
FEC Rx Descriptor Active Register—MBAR + 0x3010
The FEC descriptor active register is a command register which should be written by the user to indicate that the receive descriptor ring has 
been updated (empty receive buffers have been produced by the driver with the E bit set).
Whenever the register is written the R_DES_ACTIVE bit is set. This is independent of the data actually written by the user. When set, the 
FEC will poll the receive descriptor ring and process receive frames (provided ETHER_EN is also set). Once the FEC polls a receive 
descriptor whose ownership bit is not set, then the FEC will clear the R_DES_ACTIVE bit and cease receive descriptor ring polling until the 
user sets the bit again, signifying additional descriptors have been placed into the receive descriptor ring.
Table 14-10. FEC Interrupt Enable Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
HBEEN
BREN
BTEN
GRAEN
TFIEN
Reserved
MIIEN
Rsvd
LCEN
CRLEN
XFUNEN
XFERREN
RFERREN
Rsvd
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0
HBEEN
Heartbeat Error Interrupt Enable
1
BREN
Babbling Receiver Interrupt Enable
2
BTEN
Babbling Transmitter Interrupt Enable
3
GRAEN
Graceful Stop Interrupt Enable
4
TFIEN
Transmit Frame Interrupt Enable
5
—
Reserved
6
—
Reserved
7
—
Reserved
8
MIIEN
MII Interrupt Enable
9
—
Reserved
10
LCEN
Late Collision Enable
11
CRLEN
Late Collision Enable
12
XFUNEN
Transmit FIFO Underrun Enable
13
XFERREN
Transmit FIFO Error Enable
14
RFERREN
Receive FIFO Error Enable
15:31
—
Reserved

<!-- Slide number: 469 -->
# FEC Registers—MBAR + 0x3000

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-15
The R_DES_ACTIVE bit is cleared at reset and by the clearing of ETHER_EN.
 
14.5.5
FEC Tx Descriptor Active Register—MBAR + 0x3014
The FEC descriptor active register is a command register which should be written by the user to indicate that the transmit descriptor ring has 
been updated (transmit buffers have been produced by the driver with the R bit set in the buffer descriptor).
Whenever the register is written the X_DES_ACTIVE bit is set. This is independent of the data actually written by the user. When set, the 
FEC will poll the transmit descriptor ring and process transmit frames (provided ETHER_EN is also set). Once the FEC polls a transmit 
descriptor whose ownership bit is not set, then the FEC will clear the X_DES_ACTIVE bit and cease transmit descriptor ring polling until the 
sets the bit again, signifying additional descriptors have been placed into the transmit descriptor ring.
The X_DES_ACTIVE bit is cleared at reset and by the clearing of ETHER_EN.
 
Table 14-11. FEC Rx Descriptor Active Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
R_DES_ACTIVE
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0:6
—
Reserved
7
R_DES_ACTIVE
Set to one when this register is written, regardless of the value written. Cleared by the 
FEC device whenever no additional “ready” descriptors remain in the receive ring.
8:31
—
Reserved
Table 14-12. FEC Tx Descriptor Active Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
X_DES_ACTIVE
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0

<!-- Slide number: 470 -->
# MPC5200 Users Guide, Rev. 3.1

14-16
Freescale Semiconductor
FEC Registers—MBAR + 0x3000
14.5.6
FEC Ethernet Control Register—MBAR + 0x3024
The ECNTRL register is a read/write user register that can enable/disable the FEC. Some fields may be altered by hardware.
 
Bits
Name
Description
0:6
—
Reserved
7
X_DES_ACTIVE Set to one when this register is written, regardless of the value written. Cleared by the 
FEC device whenever no additional “ready” descriptors remain in the transmit ring.
8:31
—
Reserved
Table 14-13. FEC Ethernet Control Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
TAG0
TAG1
TAG2
TAG3
Rsvd
TESTMD
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
FEC_OE
ETHER_EN
RESET
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0:3
TAG[0:3]
This field allows programming and reading the TBUS tag bits. This field is used for 
debug/test only, and is implemented in two separate 4-bit registers. The “tags_in” 
register is written to when a sky blue write to this register takes place. This field (tags_in) 
resets to 1111. During a write cycle to any FEC register other than ECNTRL the tags_in 
value is driven onto the tbus data bus tag field. During a read cycle the tbus tag field bits 
is latched and saved in the “tags_out” register. When the ECNTRL register is read the 
value from “tags_out” shows in the TAG field.
4
—
Reserved
5
TESTMD
Test Mode—used for manufacturing test only. TESTMD resets to 0. This bit forces the 
bus controller to ignore all bus requests except the one from the SIF.
6:28
—
Reserved
29
FEC_OE
FEC Output Enable—It is a spare bit and has no affect on internal operation.
30
ETHER_EN
Ethernet Enable—When this bit is set, FEC is enabled and Rx/Tx can occur. When bit is 
cleared, Rx stops immediately; Tx stops after a bad CRC is appended to any frame 
currently being transmitted. The ETHER_EN bit is altered by hardware under the 
following conditions:
•
If ECNTRL.RESET is written to 1 by software, ETHER_EN is cleared.
•
If error conditions causing the IEVENT.EBERR, XFIFO_ERROR or RFIFO_ERROR 
bits to set occur ETHER_EN is cleared.
31
RESET
Ethernet Controller Reset—When this bit is set, the equivalent of a hardware reset is 
done, but it is local to the FEC. ETHER_EN is cleared and all other FEC registers take 
their reset values. Also, any Tx/Rx currently in progress is abruptly aborted. This bit is 
automatically cleared by hardware during the reset sequence. The reset sequence takes 
approximately 8 clock cycles after RESET is written with 1.

<!-- Slide number: 471 -->
# FEC Registers—MBAR + 0x3000

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-17
14.5.7
FEC MII Management Frame Register—MBAR + 0x3040
This MII_DATA register does not reset to a defined value. The MII_DATA register is used to communicate with the attached MII compatible 
PHY device(s), providing read/write access to the MII registers.
Writing to the MII_DATA register causes a management frame to be sourced unless the MII_SPEED register has been programmed to 0. When 
writing to MII_DATA when MII_SPEED = 0, if the MII_SPEED register is then written to a non-zero value, an MII frame is generated with 
the data previously written to the MII_DATA register. This let MII_DATA and MII_SPEED be programmed in either order if MII_SPEED is 
currently 0.
 
Note:  X: Bit does not reset to a defined value.
To do a read or write operation, the MII management interface writes to the MII_DATA register. To generate a valid read or write management 
frame:
•
the ST field must be written with a 01
•
the OP field must be written with either:
—
01 (management register write frame), or
—
10 (management register read frame), and
•
the TA field must be written with a 10
If other patterns are written to these fields, a frame is generated, but it does not comply to the IEEE 802.3 MII definition:
•
OP field = 1x produces a “read” frame operation, while
•
OP field = 0x produces a “write” frame operation.
To generate an IEEE 802.3 compliant MII management interface write frame (write to a PHY register), the user must write the following to 
the MII_DATA register:
{01 01 PHYAD REGAD 10 DATA}
Writing this pattern causes control logic to shift out the data in the MII_DATA register following a preamble generated by the control state 
machine. During this time, the MII_DATA register contents are altered as the contents are serially shifted, and is unpredictable if read by the 
Table 14-14. FEC MII Management Frame Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
ST
OP
PA
RA
TA
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
DATA
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
Bits
Name
Description
0:1
ST
Start of Frame Delimiter—bits must be programmed to 01 for a valid MII management frame.
2:3
OP
Operation Code—field must be programmed to 10 (read) or 01 (write) to generate a valid MII 
management frame. 
•
A value of 11 causes a “read” frame operation.
•
A value of 00 causes a “write” frame operation. However, these frames are not MII 
compliant.
4:8
PA
PHY Address—specifies 1 of up to 32 attached PHY devices.
9:13
RA
Register Address—specifies 1 of up to 32 registers within the specified PHY device.
14:15
TA
TurnAround—must be programmed to 10 to generate a valid MII management frame.
16:31
DATA
Management Frame Data—used for data written to or read from PHY register.

<!-- Slide number: 472 -->
# MPC5200 Users Guide, Rev. 3.1

14-18
Freescale Semiconductor
FEC Registers—MBAR + 0x3000
user. When the write management frame operation is complete, the MII_DATAIO_COMPL interrupt is generated. At this time the MII_DATA 
register contents match the original value written.
To generate an MII Management Interface read frame (read a PHY register) the user must write the following to the MII_DATA register 
(DATA field content is "don’t care"):
{01 10 PHYAD REGAD 10 XXXX}
Writing this pattern causes control logic to shift out data in the MII_DATA register following a preamble generated by the control state 
machine. During this time, the MII_DATA register contents are altered as the contents are serially shifted, and is unpredictable if read by the 
user. When the read management frame operation is complete, the MII_DATAIO_COMPL interrupt is generated. At this time the MII_DATA 
register contents matches the original value written, except for the DATA field whose contents have been replaced by the value read from the 
PHY register.
If the MII_DATA register is written while frame generation is in progress, frame contents are altered. Software should use the MII_STATUS 
register and/or the MII_DATAIO_COMPL interrupt to avoid writing to the MII_DATA register while frame generation is in process.
14.5.8
FEC MII Speed Control Register—MBAR + 0x3044
The MII_SPEED register provides MII clock (MDC pin) frequency control. This allows dropping the MII management frame preamble and 
provides observability (intended for manufacturing test) of an internal counter used in generating an MDC clock signal.
 
Table 14-15. FEC MII Speed Control Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
DIS_PREAMBLE
MII_SPEED
Rsvd
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0

<!-- Slide number: 473 -->
# FEC Registers—MBAR + 0x3000

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-19
14.5.9
FEC MIB Control Register—MBAR + 0x3064
The MIB_CONTROL register is a read/write register used to provide control of and to observe the state of the MIB block. This register is 
accessed by user software if there is a need to disable the MIB block operation. For example, to clear all MIB counters in RAM the user should 
disable the MIB block, clear all MIB RAM locations, then enable the MIB block. The MIB_DISABLE bit is reset to 1.
 
Bits
Name
Description
0:23
—
Reserved
24
DIS_PREAMBLE
Asserting this bit causes preamble (32 1s) to not be prepended to the MII 
management frame. The MII standard allows the preamble to be dropped, if not 
required by the attached PHY device(s).
25:30
MII_SPEED
Controls the frequency of the MII management interface clock (MDC) relative to 
ipb_clk. A 0 value in this field “turns off” the MDC and leaves it in low voltage state. 
Any non-zero value results in the MDC frequency of 
1/(MII_SPEED*2) of the ipb_clk frequency.
The MII_SPEED field must be programmed with a value to provide an MDC frequency 
of less than or equal to 2.5 MHz to be compliant with the IEEE MII characteristic. The 
MII_SPEED must be set to a non-zero value in order to source a read or write 
management frame. After the management frame is complete, the MII_SPEED 
register may optionally be set to 0 to turn off the MDC. The MDC generated has a 50% 
duty cycle except when MII_SPEED is changed during operation (change takes affect 
following either a rising or falling edge of MDC).
If the ipb_clk is 25MHz, programming MII_SPEED field to 0x5 results in a MDC 
frequency of 25MHz * 1/(5*2) = 2.5 MHz. Table 14-16 shows MII_SPEED optimum 
values as a function of the ipb_clk frequency.
31
—
Reserved
Table 14-16. Programming Examples for MII_SPEED Register
ipb_clk Frequency
MII_SPEED (Field in Register)
MDC Frequency
25MHz
$5
2.5MHz
33MHz
$7
2.36MHz
40MHz
$8
2.5MHz
50MHz
$A
2.5MHz
Table 14-17. FEC MIB Control Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
MIB_DISABLE
MIB_IDLE
Reserved
W
RESET:
1
1
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0

<!-- Slide number: 474 -->
# MPC5200 Users Guide, Rev. 3.1

14-20
Freescale Semiconductor
FEC Registers—MBAR + 0x3000
14.5.10
FEC Receive Control Register—MBAR + 0x3084
The R_CNTRL register is user programmable. It controls the operational mode of the receive block and should be written only when 
ETHER_EN = 0 (initialization time).
 
Bits
Name
Description
0
MIB_DISABLE
A read/write control bit. If set, MIB logic halts and MIB counters do not update.
1
MIB_IDLE
A read-only status bit. If set, MIB block is not currently updating MIB counters.
2:31
—
Reserved
Table 14-18. FEC Receive Control Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
MAX_FL
W
RESET:
0
0
0
0
0
1
0
1
1
1
1
0
1
1
1
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
FCE
BC_REJ
PROM
MII_MODE
DRT
LOOP
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
1
Bits
Name
Description
0:4
—
Reserved
5:15
MAX_FL
Maximum Frame Length—User R/W field. Resets to decimal 1518. The length is measured 
starting at DA and includes CRC at End Of Frame (EOF). Tx frames longer than MAX_FL 
causes the BABT interrupt to occur. Rx Frames longer than MAX_FL causes BABR interrupt 
to occur and sets the EOF buffer descriptor LG bit. The recommended user programmed 
default value is 1518, or if VLAN Tags are supported, 1522.
16:25
—
Reserved
26
FCE
Flow Control Enable—If asserted, the receiver detects PAUSE frames. On PAUSE frame 
detection, transmitter stops transmitting data frames for a given duration.
27
BC_REJ
Broadcast Frame Reject—If asserted, frames with DA (destination address) = 
FFFF_FFFF_FFFF are rejected, unless PROM bit is set. If both BC_REJ and PROM = 1, 
frames with broadcast DA are accepted and M (MISS) bit is set in the Rx buffer descriptor.
28
PROM
Promiscuous mode—All frames are accepted regardless of address matching.
29
MII_MODE
Selects External Interface Mode—controls the interface mode for Tx/Rx blocks.
•
Setting bit to 1 selects MII mode.
•
Setting bit to 0 selects 7wire mode (used only for serial 10Mbps).

<!-- Slide number: 475 -->
# FEC Registers—MBAR + 0x3000

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-21
14.5.11
FEC Hash Register—MBAR + 0x3088
The read-only R_HASH register provides address recognition information from the Rx block about the frame currently being received. These 
bits provide information used in the address recognition subroutine.
 
14.5.12
FEC Tx Control Register—MBAR + 0x30C4
This X_CNTRL register is read/write and is written to configure the transmit block. This register is cleared at system reset. Bits 29:30 should 
be modified only when ETHER_EN = 0.
30
DRT
Disable Receive on Transmit
0 = Rx path operates independently of Tx
(use for full-duplex or to monitor Tx activity in half-duplex mode).
1 = Disable frames reception while transmitting
(normally used for half-duplex mode).
31
LOOP
Internal Loopback—If set, transmitted frames are looped back internal to the device and 
transmit output signals are not asserted. The system clock is substituted for TX_CLK when 
LOOP is asserted. DRT must be set to 0 when asserting LOOP.
Table 14-19. FEC Hash Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
FCE_DC
MULTI
CAST
HASH
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0
FCE_DC
This is a read-only view of the R_CNTRL register FCE bit.
1
MULTICAST
Set if current Rx frame contained a multi-cast destination address, indicating DA LSB was 
set. Cleared if current Rx frame does not correspond to a multi-cast address.
2:7
HASH
Corresponds to “hash” value of current Rx frame’s destination address. Hash value is a 
6-bit field extracted from least significant portion of CRC register.
8:31
—
Reserved
Table 14-20. FEC Tx Control Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description

<!-- Slide number: 476 -->
# MPC5200 Users Guide, Rev. 3.1

14-22
Freescale Semiconductor
FEC Registers—MBAR + 0x3000
14.5.13
FEC Physical Address Low Register—MBAR + 0x30E4
The PADDR1 register is written by the user. This register contains the lower 32bits (Bytes 0,1,2,3) of the 48-bit address used in the address 
recognition process to compare with the destination address (DA) field of receive frames with an individual DA. In addition, this register is 
used in Bytes0:3 of the 6-Byte source address field when transmitting PAUSE frames. This register is not reset and must be initialized.
 
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
RFC_PAUSE
TFC_PAUSE
FDEN
HBC
GTS
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0:26
—
Reserved
27
RFC_PAUSE
This read-only status bit is asserted when a full-duplex flow control pause frame is 
received. The transmitter is paused for the duration defined in this pause frame. Bit 
automatically clears when the pause duration is complete.
28
TFC_PAUSE
Assert to transmit a PAUSE frame. When this bit is set, the MAC stops transmission of 
data frames after the current transmission is complete. At this time, the INTR_EVENT 
register GRA interrupt is asserted. With transmission of data frames stopped, the MAC 
transmits a MAC Control PAUSE frame. Next, the MAC clears the TFC_PAUSE bit and 
resumes transmitting data frames. 
Note: If the transmitter is paused due to user assertion of GTS or reception of a PAUSE 
frame, MAC may still transmit a MAC Control PAUSE frame.
29
FDEN
Full Duplex Enable—If set, frames are transmitted independent of Carrier Sense and 
Collision inputs. 
This bit should only be modified when ETHER_EN is deasserted.
30
HBC
Heartbeat Control—If set, the heartbeat check is done following End Of Transmission 
(EOT) and the Event Status Register HB bit is set if the collision input does not assert 
within the heartbeat window. 
This bit should only be modified when ETHER_EN is deasserted.
31
GTS
Graceful Transmit Stop—When this bit is set, the MAC stops transmission after any frame 
that is currently being transmitted is complete and the INTR_EVENT register GRA 
interrupt is asserted. 
If frame transmission is not currently underway, the GRA interrupt is immediately 
asserted. Once transmission completes, a “restart” can be done by clearing the GTS bit. 
The next frame in the transmit FIFO is then transmitted. 
If an early collision occurs during transmission when GTS = 1, transmission stops after 
the collision. The frame is transmitted again once GTS is cleared.
Note:  Old frames may exist in the transmit FIFO and be transmitted when GTS is 
reasserted. To avoid this, deassert ETHER_EN after the GRA interrupt.
Table 14-21. FEC Physical Address Low Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
PADDR1
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X

<!-- Slide number: 477 -->
# FEC Registers—MBAR + 0x3000

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-23
Note:  X: Bit is not reset and must be initialized.
14.5.14
FEC Physical Address High Register—MBAR + 0x30E8
The PADDR2 register is written by the user. This register contains the upper 16 bits (bytes 4 and 5) of the 48-bit address used in the address 
recognition process to compare with the destination address (DA) field of receive frames with an individual DA. In addition, this register is 
used in Bytes 4 and 5 of the 6-Byte source address field when transmitting PAUSE frames. Bits 16:31 of XMIT.PADDR2 contain a constant 
type field (hex 8808) used for transmission of PAUSE frames. This register is not reset and bits 0:15 must be initialized.
 
Note:  X: Bit is not reset and must be initialized.
14.5.15
FEC Opcode/Pause Duration Register—MBAR + 0x30EC
The OP_PAUSE register is read/write accessible. This register contains the 16-bit opcode, and 16-bit pause duration fields used in 
transmission of a PAUSE frame. The opcode field is a constant value, hex 0001. When another node detects a PAUSE frame, that node pauses 
transmission for the duration specified in the pause duration field. This register is not reset and bits 16:31 must be initialized.
 
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
PADDR1
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
Bits
Name
Description
0:31
PADDR1
Bytes 0 (bits 31:24), 1 (bits 23:16), 2 (bits 15:8) and 3 (bits 7:0) of the 6-byte individual 
address used for an exact match, and the Source Address field in PAUSE frames.
Table 14-22. FEC Physical Address High Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
PADDR2
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
TYPE
W
RESET:
1
0
0
0
1
0
0
0
0
0
0
0
1
0
0
0
Bits
Name
Description
0:15
PADDR2
Bytes 4 (bits 31:24) and 5 (bits 23:16) of the 6-byte individual address used for an exact match, 
and the Source Address field in PAUSE frames.
16:31
TYPE
These 16 bits are a constant value, hex 8808.
Table 14-23. FEC Opcode/Pause Duration Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
OPCODE
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
1

<!-- Slide number: 478 -->
# MPC5200 Users Guide, Rev. 3.1

14-24
Freescale Semiconductor
FEC Registers—MBAR + 0x3000
Note:  X: Bit is not reset and must be initialized.
14.5.16
FEC Descriptor Individual Address 1 Registe—MBAR + 0x3118
The IADDR1 register is written by the user. This register contains the upper 32 bits of the 64-bit individual address hash table used in the 
address recognition process to check for possible match with the DA field of receive frames with an individual DA. This register is not reset 
and must be initialized.
 
Note:  X: Bit is not reset and must be initialized.
14.5.17
FEC Descriptor Individual Address 2 Register—MBAR + 0x311C
The IADDR2 register is written by the user. This register contains the lower 32 bits of the 64-bit individual address hash table used in the 
address recognition process to check for possible match with the DA field of receive frames with an individual DA. This register is not reset 
and must be initialized.
 
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
PAUSE_DUR
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
Bits
Name
Description
0:15
OPCODE
Opcode field used in PAUSE frames. Bits are a constant value, hex 0001.
16:31
PAUSE_DUR
Pause Duration field used in PAUSE frames.
Table 14-24. FEC Descriptor Individual Address 1 Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
IADDR1
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
IADDR1
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
Bits
Name
Description
0:31
IADDR1
The upper 32 bits of the 64-bit hash table used in the address recognition process for receive 
frames with a unicast address. 
•
Bit 31 contains hash index bit 63.
•
Bit 0 contains hash index bit 32.
Table 14-25. FEC Descriptor Individual Address 2 Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
IADDR2
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X

<!-- Slide number: 479 -->
# FEC Registers—MBAR + 0x3000

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-25
Note:  X: Bit is not reset and must be initialized.
14.5.18
FEC Descriptor Group Address 1 Register—MBAR + 0x3120
The GADDR1 register is written by the user. This register contains the upper 32bits of the 64-bit hash table used in the address recognition 
process for receive frames with a multicast address. This register must be initialized.
 
Note:  X: Bit is not reset and must be initialized.
14.5.19
FEC Descriptor Group Address 2 Register—MBAR + 0x3124
The GADDR2 register is written by the user. The GADDR2 register contains the lower 32bits of the 64-bit hash table used in the address 
recognition process for receive frames with a multicast address. This register must be initialized.
 
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
IADDR2
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
Bits
Name
Description
0:31
IADDR2
The lower 32bits of the 64-bit hash table used in the address recognition process for receive 
frames with a unicast address. 
•
Bit 31 contains hash index bit 31.
•
Bit 0 contains hash index bit 0.
Table 14-26. FEC Descriptor Group Address 1 Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
GADDR1
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
GADDR1
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
Bits
Name
Description
0:31
GADDR1
The GADDR1 register contains the upper 32bits of the 64-bit hash table used in the address 
recognition process for receive frames with a multicast address. 
•
Bit 31 contains hash index bit 63. 
•
Bit 0 contains hash index bit 32.
Table 14-27. FEC Descriptor Group Address 2 Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
GADDR2
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X

<!-- Slide number: 480 -->
# MPC5200 Users Guide, Rev. 3.1

14-26
Freescale Semiconductor
FEC Registers—MBAR + 0x3000
Note:  X: Bit is not reset and must be initialized.
14.5.20
FEC Tx FIFO Watermark Register—MBAR + 0x3144
The X_WMRK register is a user programmable 4-bit read/write register that controls the amount of data required in the transmit FIFO before 
transmission of a frame can begin. This lets the user minimize transmit latency (X_WMRK = 0000) or allows for larger bus access latency 
(X_WMRK = 1111) due to contention for the system bus. Setting the watermark to a high value minimizes the risk of transmit FIFO underrun 
due to contention for the system bus. The X_WMRK register resets to 0.
NOTE
This register value may need to be customized by software for specific FEC applications to be 
compatible with specific FIFO/system bus access latency requirements.
 
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
GADDR2
W
RESET:
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
X
Bits
Name
Description
0:31
GADDR2
The GADDR2 register contains the lower 32bits of the 64-bit hash table used in the address 
recognition process for receive frames with a multicast address.
•
Bit 31 contains hash index bit 31. 
•
Bit 0 contains hash index bit 0.
Table 14-28. FEC Tx FIFO Watermark Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
X_WMRK
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0

<!-- Slide number: 481 -->
# FIFO Interface

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-27
14.6
FIFO Interface
The programming interface to the FIFO allows access to Data, Status, Control, Last Write Pointer, Last Read Pointer, Alarm, Read and Write 
Pointers for Transmit and Receive configurations. The FIFO can be accessed by byte, word, or longword, but all accesses must be aligned 
with the most significant byte (big endian) of the data port. BestComm supports byte, word or longword accesses. The processor supports 
longword access only. All register name access is longword aligned
.
Bits
Name
Description
0:28
—
Reserved
28:31
X_WMRK
Transmit FIFO Watermark—Frame transmission begins: 
•
If the number of bytes selected by this field are written into the transmit FIFO, or 
•
if an EOF is written to the FIFO, or 
•
if the FIFO is full before the selected number of bytes are written.
Options are:
0000 = 64Bytes written to FIFO.
0001 = 128Bytes written to FIFO.
0010 = 192Bytes written to FIFO.
0011 = 256Bytes written to FIFO.
0100 = 320Bytes written to FIFO.
0101 = 384Bytes written to FIFO.
0110 = 448Bytes written to FIFO.
0111 = 512Bytes written to FIFO.
1000 = 576Bytes written to FIFO.
1001 = 640Bytes written to FIFO.
1010 = 704Bytes written to FIFO.
1011 = 768Bytes written to FIFO.
1100 = 832Bytes written to FIFO.
1101 = 896Bytes written to FIFO.
1110 = 960Bytes written to FIFO.
1111 = 1024Bytes written to FIFO.
Table 14-29. FIFO Interface Register Map
Address
byte0
byte1
byte2
byte3
Description
0x184
Data
Data
Data
Data
Receive FIFO Data
0x188
Stat
Stat
Receive FIFO Status
0x18C
Ctl
Receive FIFO Control
0x190
LRF
LRF
Receive Last Read Frame Pointer
0x194
LWF
LWF
Receive Last Write Frame Pointer
0x198
Alarm
Alarm
Receive (High/Low) Alarm Pointer
0x19C
Read
Read
Receive FIFO Read Pointer
0x1A0
Write
Write
Receive FIFO Write Pointer
0x1A4
Data
Data
Data
Data
Transmit FIFO Data
0x1A8
Stat
Stat
Transmit FIFO Status
0x1AC
Ctl
Transmit FIFO Control
0x1B0
LRF
LRF
Transmit Last Read Frame Pointer

<!-- Slide number: 482 -->
# MPC5200 Users Guide, Rev. 3.1

14-28
Freescale Semiconductor
FEC Tx FIFO Data Register—MBAR + 0x31A4
14.6.1
FEC Rx FIFO Data Register—MBAR + 0x3184
14.7
FEC Tx FIFO Data Register—MBAR + 0x31A4
The RFIFO_DATA and TFIFO_DATA registers are the main interface port for the Transmit and Receive FIFO. Data which is to be buffered 
in the FIFO, or has been buffered in the FIFO, is accessed through this register.
14.7.1
FEC Rx FIFO Status Register—MBAR + 0x3188
14.8
FEC Tx FIFO Status Register—MBAR + 0x31A8
The RFIFO_STATUS and TFIFO_STATUS registers contain bits which provide information about the status of the FIFO controller. The bits 
marked sticky are cleared by writing a ‘1’ to their positions.
 
0x1B4
LWF
LWF
Transmit Last Write Frame Pointer
0x1B8
Alarm
Alarm
Transmit (High/Low) Alarm Pointer
0x1BC
Read
Read
Transmit FIFO Read Pointer
0x1C0
Write
Write
Transmit FIFO Write Pointer
Table 14-30. FEC Rx FIFO Status Register
FEC Tx FIFO Status Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
Frame[0:3]
Rsvd
Error
UF
OF
FR
Full
Alarm
Empty
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
1
1
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
1
1
Bits
Name
Description
0:3
—
Reserved
4:7
Frame[0:3]
Frame Indicator – READ ONLY
This bus provides a frame status indicator for non-DMA applications.
Frame[0] = A frame boundary has occurred on the [31:24] byte of the data bus.
Frame[1] = A frame boundary has occurred on the [23:16] byte of the data bus.
Frame[2] = A frame boundary has occurred on the [15:8] byte of the data bus.
Frame[3] = A frame boundary has occurred on the [7:0] byte of the data bus.
8
---
Reserved
9
Error
FIFO Error – Sticky, Write To Clear.
This bit signifies that an error has occurred in the FIFO controller. Errors can be caused by 
underflow, overflow,or pointers being out of bounds. This bit will remain set until this bit of the 
FIFO status register has been written with a 1.
Table 14-29. FIFO Interface Register Map (continued)
Address
byte0
byte1
byte2
byte3
Description

<!-- Slide number: 483 -->
# FEC Tx FIFO Status Register—MBAR + 0x31A8

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-29
14.8.1
FEC Rx FIFO Control Register—MBAR + 0x318C
FEC Tx FIFO Control Register—MBAR + 0x31AC
The RFIFO_CONTROL and TFIFO_CONTROL registers provide programmability of many FIFO behaviors, from last transfer granularity 
to frame operation. Last transfer granularity allows the user to control when the FIFO controller stops requesting data transfers through the 
FIFO alarm. When the alarm is configured as a Receive FIFO, the granularity value is the GR[2:0] value. When the alarm is configured as a 
Transmit FIFO, the granularity value is four times the GR[2:0] value, or the pipeline depth. The frame bit of the control register provides a 
capability to enable and control the FIFO controller’s ability to view data on a packetized basis. The FIFO controller also has the 
programmable capability to not request attention after it has received a complete frame until Ethernet has reported completion of transmission. 
Frame mode supersedes the FIFO granularity bits, through the assertion of a hardware signal to BestComm.
10
UF
UF FIFO Underflow – Sticky, Write To Clear
This bit signifies the read pointer has surpassed the write pointer. This bit will remain set until 
this bit of the FIFO status register has been written with a 1.
11
OF
OF FIFO Overflow – Sticky, Write To Clear
This bit signifies the write pointer has surpassed the read pointer. This bit will remain set until 
this bit of the FIFO status register has been written with a 1.
12
FR
FR Frame Ready – Read Only
The FIFO has requested attention because there is framed data ready. All complete frames 
must be read from the FIFO to clear this alarm. This alarm will only be asserted while in 
frame mode.
13
Full
Full Alarm – Read Only
The FIFO has requested attention because it is full. The FIFO must be read to clear this 
alarm.
14
Alarm
FIFO Alarm – Read Only
The FIFO has requested attention because it has determined an alarm condition. The 
specific alarm condition detected is dependent upon the FIFO direction (Transmit or 
Receive); if it is a Transmit FIFO, then the FIFO alarm output pin provides indication of a low 
level, asserting when there is less than alarm bytes of data remaining in the FIFO, and 
deasserting when there are less than 4* granularity free bytes remaining. When the FIFO is 
configured to Receive, the FIFO alarm provides high level indication, asserting when there 
are less than alarm bytes free in the FIFO, and deasserting when there are less than 
granularity bytes of data remaining. This signal can be cleared by reading or writing (as 
appropriate) the FIFO, or manipulating the FIFO pointers.
15
Empty
Empty – Read Only
The FIFO has requested attention because it is empty. The FIFO must be written to clear this 
alarm.
16:31
---
Reserved
Bits
Name
Description

<!-- Slide number: 484 -->
# MPC5200 Users Guide, Rev. 3.1

14-30
Freescale Semiconductor
FEC Tx FIFO Status Register—MBAR + 0x31A8
 
14.8.2
FEC Rx FIFO Last Read Frame Pointer Register—MBAR + 0x3190
FEC Tx FIFO Last Read Frame Pointer Register—MBAR + 0x31B0
The RFIFO_LRF_PTR and TFIFO_LRF_PTR are a FIFO-maintained pointer which indicates the location of the start of the most recently 
read frame, or the start of the frame currently in transmission. The LRFP updates on FIFO read data accesses to a frame boundary. The LRFP 
can be read and written for debug purposes. For the frame retransmit function, the LRFP indicates which point to begin retransmission of the 
data frame. The LRFP carries validity information, however, there are no safeguards to prevent retransmitting data which has been 
overwritten. When FRAME is not set, then this pointer has no meaning.
 
Table 14-31. FEC Rx FIFO Control Register 
FEC Tx FIFO Control Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Rsvd
WFR[1:0]
COMP
FRAME
GR[2:0]
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0
—
Reserved
1:2
WFR[1:0]
Write Frame
01 = the FIFO controller assumes the next write to its data port is the next to last write.
10 = the FIFO controller assumes the next write to its data port is status / control 
information.
3
COMP
COMP Re-enable Requests on Frame Transmission Completion.
When this bit is set, the FIFO controller will not request attention between receiving the last 
data of the frame from the BestComm until the peripheral acknowledges transmission of the 
frame.
4
FRAME
Frame Mode Enable.
When this bit is set, the FIFO controller monitors frame done information from the peripheral 
or BestComm. Setting this bit also enables the other frame control bits in this register, as well 
as other frame functions. This bit must be set to use frame functions.
5:7
GR[2:0]
Last Transfer Granularity.
These bits define the deassertion point for the “high” service request and also define the 
deassertion point for the “low” service request. A “high” service request is deasserted when 
there are less than GR[2:0] data bytes remaining in the FIFO. A “low” service request is 
deasserted when there are less than (4 * GR[2:0]) free bytes remaining in the FIFO.
Table 14-32. FEC Rx FIFO Last Read Frame Pointer Register
FEC Tx FIFO Last Read Frame Pointer Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0

<!-- Slide number: 485 -->
# FEC Tx FIFO Status Register—MBAR + 0x31A8

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-31
14.8.3
FEC Rx FIFO Last Write Frame Pointer Register—MBAR + 0x3194
FEC Tx FIFO Last Write Frame Pointer Register—MBAR + 0x31B4
The RFIFO_LWF_PTR and TFIFO_LWF_PTR are a FIFO maintained pointer which indicates the location of the start of the last frame written 
into the FIFO. The LWFP updates on FIFO write data accesses which create a frame boundary, whether that be by setting the writeFrameCtl 
control bit, or by feeding a frame bit in on the appropriate bus. The LWFP can be read and written for debug purposes. For the frame discard 
function, the LWFP divides the valid data region of the FIFO (the area in-between the read and write pointers) into framed and unframed data. 
Data between the LWFP and write pointer constitutes an incomplete frame, while data between the read pointer and the LWFP has been 
received as whole frames. When FRAME is not set, then this pointer has no meaning.
 
14.8.4
FEC Rx FIFO Alarm Pointer Register—MBAR + 0x3198
FEC Tx FIFO Alarm Pointer Register—MBAR + 0x31B8
RFIFO_ALARM and TFIFO_ALARM include pointer which provide high/low level alarm information to the user integration logic and the 
BestComm interface. A low level alarm reports lack of data; a high level alarm reports lack of space. The alarm pointer is interpreted 
depending on the state of the FIFO transmit input pin: if FIFO transmit = ‘1’, then the alarm is represented in terms of data bytes, if FIFO 
Transmit = ‘0’, the alarm is represented in terms of free bytes. This programmable alarm can warn the system when the FIFO is almost full 
of data (FIFO Transmit = ‘0’), or when the FIFO is almost out of data (FIFO Transmit = ‘1’). This register is programmed to the upper limit 
for the number of bytes in the FIFO of data, when FIFO transmit is negated, or space, when FIFO transmit is asserted, before an internal alarm 
is set. Any time the amount of data or space in the FIFO is above the indicated amount, the alarm will be set. The alarm is cleared when there 
is less data or space than is defined as the FIFO granularity or pipeline depth. The number of bits in the alarm pointer register will vary with 
the address space of the FIFO memory, and the alarm pointer is initialized to zero.
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
LRFP[9:0]
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0:21
—
Reserved
22:31
LRFP[9:0]
LRFP Last Read Frame Pointer.
This pointer indicates the start of the last data frame read from the FIFO by the peripheral.
Table 14-33. FEC Rx FIFO Last Write Frame Pointer Register
FEC Tx FIFO Last Write Frame Pointer Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
LRFP[9:0]
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0:21
—
Reserved
22:31
LWFP[9:0]
LRFP Last WriteFrame Pointer.
This pointer indicates the start of the last data frame written into the FIFO by the peripheral.

<!-- Slide number: 486 -->
# MPC5200 Users Guide, Rev. 3.1

14-32
Freescale Semiconductor
FEC Tx FIFO Status Register—MBAR + 0x31A8
 
14.8.5
FEC Rx FIFO Read Pointer Register—MBAR + 0x319C
FEC Tx FIFO Read Pointer Register—MBAR + 0x31BC
The RFIFO_RDPTR and TFIFO_RDPTR are a FIFO-maintained pointer which point to the next FIFO location to be read. The read pointer 
can be both read and written.
 
Table 14-34. FEC Rx FIFO Alarm Pointer Register
FEC Tx FIFO Alarm Pointer Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
Alarm[9:0]
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0:21
—
Reserved
22:31
Alarm[9:0]
Alarm Pointer.
This pointer indicates the point at (or below) which to assert the FIFO alarm signal. This 
value is compared with data or free bytes, depending upon the state of FIFO Transmit (FIFO 
Transmit = ‘1’, alarm measures data bytes).
Table 14-35. FEC Rx FIFO Read Pointer Register
FEC Tx FIFO Read Pointer Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
READ[9:0]
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0:21
—
Reserved
22:31
READ[9:0]
Read Pointer.
This pointer indicates the next location to be read by the FIFO controller.

<!-- Slide number: 487 -->
# FEC Tx FIFO Status Register—MBAR + 0x31A8

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-33
14.8.6
FEC Rx FIFO Write Pointer Register—MBAR + 0x31A0
FEC Tx FIFO Writer Pointer Register—MBAR + 0x31C0
The RFIFO_WRPTR and TFIFO_WRPTR are a FIFO-maintained pointer which point to the next FIFO location to be written. The write 
pointer can be both read and written.
 
14.8.7
FEC Reset Control Register—MBAR + 0x31C4
The RESET_CNTRL register allows reset of the FIFO controllers.
 
Table 14-36. FEC Rx FIFO Write Pointer Register
FEC Tx FIFO Write Pointer Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
WRITE[9:0]
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0:21
—
Reserved
22:31
WRITE[9:0]
WRITE Pointer.
This pointer indicates the next location to be written by the FIFO controller.
Table 14-37. FEC Reset Control Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
RCTL[1]
RCTL[0]
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET
:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Table 1-1. 
Bits
Name
Description
0:5
—
Reserved
6
RCTL[1]
0 = Do not Reset FIFO controllers.
1 = Reset FIFO controllers.

<!-- Slide number: 488 -->
# MPC5200 Users Guide, Rev. 3.1

14-34
Freescale Semiconductor
Initialization Sequence
14.8.8
FEC Transmit FSM Register—MBAR + 0x31C8
The transmit finite state machine register (XMIT_FSM) controls operation of appending CRC. Typical use is enabled and CRC appended.
 
14.9
Initialization Sequence
This section describes which registers are hardware reset, which are reset by the FEC, and what locations the user must initialize prior to 
enabling the FEC.
14.9.1
Hardware Controlled Initialization
Some registers in the FEC are reset by internal logic. Specifically those registers are control logic that generate interrupts, cause outputs to be 
asserted and in general, configuration control bits.
Other registers are reset when the ETHER_EN bit is not asserted (i.e., cleared). To halt operation ETHER_EN is deasserted by either a hard 
reset or by software. By deasserting ETHER_EN configuration control registers such as X_CNTRL and R_CNTRL are not reset, but the entire 
data path is reset.
Table 14-39 shows the effect deasserting ETHER_EN has on Ethernet MAC operation and registers.
7
RCTL[0]
0 = Disable fec_enable as a reset to FIFO controllers.
1 = Enable fec_enable as a reset to FIFO controllers.
8:31
---
Reserved
Table 14-38. FEC Transmit FSM Register
msb 0
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
R
Reserved
XFSM[1]
XFSM[0]
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
16
17
18
19
20
21
22
23
24
25
26
27
28
29
30
31 lsb
R
Reserved
W
RESET:
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
0
Bits
Name
Description
0:5
—
Reserved
6
XFSM[1]
0 = Do not append CRC.
1 = Append CRC (typical use).
7
XFSM[0]
0 = Disable CRC FSM.
1 = Enable CRC FSM (typical use is enabled).
8:31
---
Reserved
Table 14-39. ETHER_EN De-Assertion Affect on FEC
Register/Machine
Reset Value
XMIT block
Transmission Aborted (bad CRC appended)
RECV block
Receive activity aborted
Tx/Rx FIFO
Reset control logic dependent on reset_cntrl
Table 1-1. 
Bits
Name
Description

<!-- Slide number: 489 -->
# Initialization Sequence

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-35
14.9.2
User Initialization (Prior to Asserting ETHER_EN)
The user needs to initialize portions of the FEC prior to setting the ETHER_EN bit. The exact values depend on the particular application; the 
sequence of writing the registers is not important. Ethernet MAC registers requiring initialization are defined in Table 14-40.
14.9.2.1
Microcontroller Initialization
In the FEC the descriptor control RISC initializes some registers after ETHER_EN is asserted. After the Microcontroller initialization 
sequence is complete, hardware is ready for operation.
Table 14-41 shows RISC initialization operations common to the FEC.
14.9.3
Frame Control/Status Words 
In the FEC transmit frame control words and receive frame status words cross the following the end of frame data. These words are marked 
with a type value of 10 and have the following formats.
14.9.3.1
Receive Frame Status Word
Table 14-2 below defines the format for the receive frame status word. 
Bits 31-28, 26-25, 19 and 15-11—Reserved
Table 14-40. User Initialization (Before ETHER_EN)
Description
Initialize IMASK
Clear IEVENT (write FFFF_FFFF)
X_WMRK (optional)
IADDR2/IADDR1
GADDR1/GADDR2
PADDR1/PADDR2
OP_PAUSE (only needed for FDX flow control)
R_CNTRL
X_CNTRL
MII_SPEED (optional)
Clear MIB_RAM (locations 200–2FC)
Table 14-41. Microcontroller Initialization (FEC)
Description
Initialize BackOff random number seed
Activate Receiver
Activate Transmit
Table 14-42. Receive Frame Status Word Format
31
30
29
28
27
26
25
24
23
22
21
20
19
18
17
16
0
0
0
0
1
(Last)
0
0
M
BC
MC
LG
NO
0
CR
OV
TR
15
14
13
12
11
10
9
8
7
6
5
4
3
2
1
0
0
0
0
0
0
FRAME_LENGTH

<!-- Slide number: 490 -->
# MPC5200 Users Guide, Rev. 3.1

14-36
Freescale Semiconductor
Initialization Sequence
L—Last in Frame, written by the FEC
The buffer is not the last in a frame.
The buffer is the last in a frame.
BC—Will be set if the DA is broadcast (FF-FF-FF-FF-FF-FF)
MC—Will be set if the DA is multicast and not BC
LG—Rx Frame Length Violation, written by the FEC.
A frame length greater than R_CNTRL.MAX_FL was recognized. This bit is valid only if the L-bit is set. The receive data is not altered 
in any way unless the length exceeds 2047 bytes.
NO—Rx Non-octet Aligned Frame, written by the FEC.
A frame that contained a number of bits not divisible by 8 was received, and the CRC check that occurred at the preceding byte boundary 
generated an error. This bit is valid only if the L-bit is set. If this bit is set the CR bit will not be set.
CR—Rx CRC Error, written by the FEC.
This frame contains a CRC error and is an integral number of octets in length. This bit is valid only if the L-bit is set.
OV—Overrun, written by the FEC.
A receive FIFO overrun occurred during frame reception. If this bit is set, the other status bits, M, LG, NO, SH, CR, and CL lose their 
normal meaning and will be zero. This bit is valid only if the L-bit is set.
TR—Rx Frame Truncated
Will be set if the receive frame is truncated (frame length > 2047 bytes). If the TR bit is set the frame should be discarded and the other 
error bits should be ignored as they may be incorrect.
FRAME_LENGTH— Length of Received Frame
14.9.3.2
Transmit Frame Control Word
The only requirement for this control word is to have the TC and ABC bits valid. The TC bit defines whether the transmit block should append 
the CRC (TC = 1) or not (TC = 0) for the current frame. The ABC bit defines whether the transmit block should append a bad CRC (ABC = 
1), independent of the TC value. Refer to Table 14-43 below for the format of the transmit frame control word. 
Bits 31-27, 24-0—Reserved
TC—Transmit CRC, written by user
0 = End transmission immediately after the last data byte.
1 = Transmit the CRC sequence after the last data byte.
ABC—Append Bad CRC, written by user
0 = No affect
1 = Transmit the CRC sequence inverted after the last data bye (regardless of TC value).
14.9.4
Network Interface Options
The FEC supports both an MII interface for 10/100 Mbps Ethernet and a 7-wire serial interface for 10 Mbps Ethernet. The interface mode is 
selected by the MII_MODE bit in the R_CNTRL register. In MII mode (R_CNTRL.MII_MODE = 1), there are 18 signals defined by the 
802.3 standard and supported by the FEC. These are shown in Table 14-1:
The 7-Wire serial interface (R_CNTRL.MII_MODE = 0) operates in what is generally referred to as the “AMD” mode. FEC Frame 
Transmission
Table 14-43. Transmit Frame Control Word Format
31
30
29
28
27
26
25
24
23
22
21
20
19
18
17
16
TC
ABC
15
14
13
12
11
10
9
8
7
6
5
4
3
2
1
0

<!-- Slide number: 491 -->
# Initialization Sequence

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-37
The Ethernet transmitter is designed to work with almost no intervention from software. Once ETHER_EN is asserted and data appears in the 
transmit FIFO the Ethernet MAC is able to transmit onto the network.
When the transmit FIFO fills to the watermark (defined by the X_WMRK register), the MAC transmit logic will assert TX_EN and start 
transmitting the preamble sequence, the start frame delimiter, and then the frame information from the FIFO. However, the controller defers 
the transmission if the network is busy (carrier sense is asserted). Before transmitting, the controller waits for carrier sense to become inactive, 
then determines if carrier sense stays inactive for 60 bit times. If so, then the transmission begins after waiting an additional 36 bit times (96 
bit times after carrier sense originally became inactive).
If a collision occurs during transmission of the frame (half-duplex mode), the ethernet controller follows the specified backoff procedures and 
attempts to retransmit the frame until the retry limit is reached. The transmit FIFO stores at least the first 64 bytes of the transmit frame, so 
that they do not have to be retrieved from system memory in case of a collision. This improves bus utilization and latency in case immediate 
retransmission is necessary.
When all the frame data has been transmitted, the FCS (32-bit CRC) bytes are appended if the TC bit is set in the transmit frame control word. 
If the ABC bit is set in the transmit frame control word, a bad CRC will be appended to the frame data regardless of the TC bit value. Following 
the transmission of the CRC, the ethernet controller writes the frame status information to the MIB block. Short frames are automatically 
padded by the transmit logic (if the TC bit in the transmit buffer descriptor for the end of frame buffer = 1).
The FEC frame interrupts may be generated as determined by the settings in the IMASK register.
Transmit error interrupts are HBERR, BABT, LATE_COL, COL_RETRY_LIM, XFIFO_UN and XFIFO_ERROR. If the transmit frame 
length exceeds MAX_FL bytes the BABT interrupt will be asserted, however the entire frame will be transmitted (no truncation).
To pause transmission, set the GTS (Graceful Transmit Stop) bit in the X_CNTRL register. When the GTS is set the FEC transmitter stops 
immediately if transmission is not in progress; otherwise, it continues transmission until the current frame either finishes or terminates with 
a collision. After the transmitter has stopped the GRA (Graceful Stop Complete) interrupt is asserted. If GTS is cleared, the FEC resumes 
transmission with the next frame. 
The ethernet controller transmits bytes least significant bit first.
14.9.5
FEC Frame Reception
The FEC receiver is designed to work with almost no intervention from the host and can perform address recognition, CRC checking, short 
frame checking and maximum frame length checking.
When the driver enables the FEC receiver by asserting ETHER_EN it will immediately start processing receive frames. When RX_DV asserts, 
the receiver will first check for a valid PA/SFD header. If the PA/SFD is valid it will be stripped and the frame will be processed by the receiver. 
If a valid PA/SFD is not found the frame will be ignored. 
In 7-wire serial mode, the first 16 bit times of RX_D0 following assertion of RX_DV (RENA) are ignored. Following the first 16 bit times 
the data sequence is checked for alternating 1s and 0s. If a 11 or 00 data sequence is detected during bit times 17 to 21, the remainder of the 
frame is ignored. After bit time 21, the data sequence is monitored for a valid SFD (11). If a 00 is detected, the frame is rejected. When a 11 
is detected, the PA/SFD sequence is complete. 
In MII mode the receiver checks for at least one byte matching the SFD. Zero or more PA bytes may occur, but if a 00 bit sequence is detected 
prior to the SFD byte, the frame is ignored.
After the first 6 bytes of the frame have been received, the FEC performs address recognition on the frame. 
Once a collision window (64 bytes) of data has been received and if address recognition has not rejected the frame, the receive FIFO is 
signalled that the frame is “accepted” and may be passed on to the DMA. If the frame is a runt (due to collision) or is rejected by address 
recognition, the receive FIFO is notified to “reject” the frame. Thus, no collision fragments are presented to the user except late collisions, 
which indicate serious LAN problems. 
During reception, the ethernet controller checks for various error conditions and once the entire frame is written into the FIFO, a 32-bit frame 
status word is written into the FIFO. This status word contains the M, BC, MC, LG, NO, SH, CR, OV and TR status bits, and the frame length.
The ethernet controller receives serial data LSB first.
14.9.6
Ethernet Address Recognition
The FEC filters the received frames based on destination address (DA) type — individual (unicast), group (multicast) or broadcast (all-ones 
group address). The difference between an individual address and a group address is determined by the I/G bit in the destination address field. 
A flowchart for address recognition on received frames is illustrated in the figures below.
Address recognition is accomplished through the use of the receive block and microcode running on the microcontroller. The flowchart shown 
in Figure14-4 illustrates the address recognition decisions made by the receive block, while Figure 14-5 illustrates the decisions made by the 
microcontroller.

<!-- Slide number: 492 -->
# MPC5200 Users Guide, Rev. 3.1

14-38
Freescale Semiconductor
Initialization Sequence
If the DA is a broadcast address and broadcast reject (R_CNTRL.BC_REJ) is deasserted, then the frame will be accepted unconditionally as 
shown in Figure 14-4. Otherwise, if the DA in not a broadcast address the microcontroller runs the address recognition subroutine as shown 
in Figure 14-5. 
If the DA is a group (multicast) address and flow control is disabled the microcontroller will perform a group hash table lookup using the 
64-entry hash table programmed in GADDR1 and GADDR2. If a hash match occurs AR_HM_B (address recognition hash match bar) is set 
to 0 and the receiver accepts the frame. If flow control is enabled the microcontroller will do an exact address match check between the DA 
and the designated PAUSE DA in registers XMIT.FDXFC_DA1 and XMIT.FDXFC_DA2. In the case where a PAUSE DA exact match occurs 
AR_EM_B (address recognition exact match bar) is set to 0. If the receive block determines that the received frame is a valid PAUSE frame 
the frame will be rejected. Note the receiver will detect a PAUSE frame with the DA field set to either the designated PAUSE DA or the unicast 
physical address.
If the DA is the individual (unicast) address the microcontroller performs an individual exact match comparison between the DA and 48-bit 
physical address that the user programs in the PADDR1 and PADDR2 registers. If an exact match occurs AR_EM_B is set to 0; otherwise, 
the microcontroller does an individual hash table lookup using the 64-entry hash table programmed in registers IADDR1 and IADDR2. In the 
case of an individual hash match AR_HM_B is set to 0. Again, the receiver will accept or reject the frame based on PAUSE frame detection, 
shown in Figure 14-4.
If neither a hash match (group or individual) nor an exact match (group or individual) occur both AR_HM_B and AR_EM_B are set to 1. In 
this case, if promiscuous mode is enabled (R_CNTRL.PROM = 1), then the frame will be accepted and the MISS bit in the receive buffer 
descriptor is set; otherwise, the frame will be rejected and the MISS bit will be cleared.
Similarly, if the DA is a broadcast address, broadcast reject (R_CNTRL.BC_REJ) is asserted and promiscuous mode is enabled. Then the 
frame will be accepted and the MISS bit in the receive buffer descriptor is set; otherwise, the frame will be rejected and the MISS bit will be 
cleared.
In general, when a frame is rejected it is flushed from the FIFO.

<!-- Slide number: 493 -->
# Initialization Sequence

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-39
Figure 14-2. Ethernet Address Recognition - receive block decisions
Accept/Reject
Broadcast Addr
?
?
PROM = 1
?
Receive
Address
True
NOTES:
BC_REJ - field in R_CNTRL register (BroadCast REJect)
False
True
 
False
BC_REJ = 1
?
AR_EM_B - bit in RECV.AR_DONE register (address recognition exact match bar)
AR_HM_B - bit in RECV.AR_DONE register (address recognition hash match bar)
Frame
AR_HM_B = 0
?
AR_EM_B = 0
?
Pause Frame
False
False
False
False
True
True
True
True
Receive Frame
Receive Frame
Receive Frame
Receive Frame
Reject Frame
Reject Frame
PROM - field in R_CNTRL register (PROMiscous mode)
Pause Frame - valid PAUSE frame received
Check Address - microcode Address Recognition subroutine; returns AR_HM_B and AR_EM_B
Set BC bit in RCV BD
Set MC bit in RCV BD if multicast
Set M (Miss) bit in Rcv BD
Set MC bit in Rcv BD if multicast
Set BC bit in Rcv BD if broadcast
Flush from FIFO
Flush from FIFO
Recognition

<!-- Slide number: 494 -->
# MPC5200 Users Guide, Rev. 3.1

14-40
Freescale Semiconductor
Initialization Sequence
The hash table algorithm used in the group and individual hash filtering operates as follows. The 48-bit destination address is mapped into 
one of 64 bits which are represented by 64 bits stored in GADDR1,2 (group address hash match) or IADDR1,2 (individual address hash 
match). This mapping is performed by passing the 48-bit address through the on-chip 32-bit CRC generator and selecting the 6 most 
significant bits of the CRC-encoded result to generate a number between 0 and 63. The MSB of the CRC result selects GADDR1 (MSB = 1) 
or GADDR2 (MSB = 0). The least significant 5 bits of the hash result select the bit within the selected register. If the CRC generator selects 
a bit that is set in the hash table, the frame is accepted; otherwise, it is rejected. 
For example, if eight group addresses are stored in the hash table and random group addresses are received, the hash table prevents roughly 
56/64 (or 87.5%) of the group address frames from reaching memory. Those that do reach memory must be further filtered by the processor 
to determine if they truely contain one of the eight desired addresses. 
The effectiveness of the hash table declines as the number of addresses increases.
The hash table registers must be initialized by the user. The user may compute the hash for a particular address in software. The CRC32 
polynomial to use in computing the hash is:
A table of example Destination Addresses and corresponding hash values is included below for reference.
Figure 14-3. Ethernet Address Recognition - microcode decisions
Receive Address
I/G Address
?
Exact Match
?
Hash Search
Group Table
Match
?
Hash Search
Individual Table
False
Match
?
False
False
True
True
True
NOTES:
FCE - field in R_CNTRL register (Flow Control Enable)
I/G - Individual/Group bit in Destination Address (least significant bit in first byte received in MAC frame)
Individual
Group
ar_em_b = 0
ar_hm_b = 1
 
True
ar_em_b = 0
ar_hm_b = 1
ar_em_b = 1
ar_hm_b = 1
ar_em_b = 1
ar_hm_b = 0
False
True
False
?
Pause Address
FCE
?
ar_em_b = 1
ar_hm_b = 0
ar_em_b = 1
ar_hm_b = 1
AR_EM_B - bit in RECV.AR_DONE register (address recognition exact match bar)
AR_HM_B - bit in RECV.AR_DONE register (address recognition hash match bar)
Recognition
X32
X26
X23
X22
X16
X12
X11
X10
X8
X7
X5
X4
X2
X
1
+
+
+
+
+
+
+
+
+
+
+
+
+
+

<!-- Slide number: 495 -->
# Initialization Sequence

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-41
Table 14-44. Destination Address to 6-Bit Hash
48-bit DA
6-bit hash (in hex)
hash decimal value
65:ff:ff:ff:ff:ff
0x0
0
55:ff:ff:ff:ff:ff
0x1
1
15:ff:ff:ff:ff:ff
0x2
2
35:ff:ff:ff:ff:ff
0x3
3
b5:ff:ff:ff:ff:ff
0x4
4
95:ff:ff:ff:ff:ff
0x5
5
d5:ff:ff:ff:ff:ff
0x6
6
f5:ff:ff:ff:ff:ff
0x7
7
db:ff:ff:ff:ff:ff
0x8
8
fb:ff:ff:ff:ff:ff
0x9
9
bb:ff:ff:ff:ff:ff
0xa
10
8b:ff:ff:ff:ff:ff
0xb
11
0b:ff:ff:ff:ff:ff
0xc
12
3b:ff:ff:ff:ff:ff
0xd
13
7b:ff:ff:ff:ff:ff
0xe
14
5b:ff:ff:ff:ff:ff
0xf
15
27:ff:ff:ff:ff:ff
0x10
16
07:ff:ff:ff:ff:ff
0x11
17
57:ff:ff:ff:ff:ff
0x12
18
77:ff:ff:ff:ff:ff
0x13
19
f7:ff:ff:ff:ff:ff
0x14
20
c7:ff:ff:ff:ff:ff
0x15
21
97:ff:ff:ff:ff:ff
0x16
22
a7:ff:ff:ff:ff:ff
0x17
23
99:ff:ff:ff:ff:ff
0x18
24
b9:ff:ff:ff:ff:ff
0x19
25
f9:ff:ff:ff:ff:ff
0x1a
26
c9:ff:ff:ff:ff:ff
0x1b
27
59:ff:ff:ff:ff:ff
0x1c
28
79:ff:ff:ff:ff:ff
0x1d
29
29:ff:ff:ff:ff:ff
0x1e
30
19:ff:ff:ff:ff:ff
0x1f
31
d1:ff:ff:ff:ff:ff
0x20
32
f1:ff:ff:ff:ff:ff
0x21
33
b1:ff:ff:ff:ff:ff
0x22
34

<!-- Slide number: 496 -->
# MPC5200 Users Guide, Rev. 3.1

14-42
Freescale Semiconductor
Initialization Sequence
14.9.7
Full-Duplex Flow Control
Full-duplex flow control allows the user to transmit pause frames and to detect received pause frames. Upon detection of a pause frame, MAC 
data frame transmission stops for a given pause duration.
To enable pause frame detection, the FEC must operate in full-duplex mode (X_CNTRL.FDEN asserted) and flow control enable 
(R_CNTRL.FCE) must be asserted. The FEC detects a pause frame when the fields of the incoming frame match the pause frame 
specifications as shown in the table below. In addition, the receive status associated with the frame should indicate that the frame is valid
91:ff:ff:ff:ff:ff
0x23
35
11:ff:ff:ff:ff:ff
0x24
36
31:ff:ff:ff:ff:ff
0x25
37
71:ff:ff:ff:ff:ff
0x26
38
51:ff:ff:ff:ff:ff
0x27
39
7f:ff:ff:ff:ff:ff
0x28
40
4f:ff:ff:ff:ff:ff
0x29
41
1f:ff:ff:ff:ff:ff
0x2a
42
3f:ff:ff:ff:ff:ff
0x2b
43
bf:ff:ff:ff:ff:ff
0x2c
44
9f:ff:ff:ff:ff:ff
0x2d
45
df:ff:ff:ff:ff:ff
0x2e
46
ef:ff:ff:ff:ff:ff
0x2f
47
93:ff:ff:ff:ff:ff
0x30
48
b3:ff:ff:ff:ff:ff
0x31
49
f3:ff:ff:ff:ff:ff
0x32
50
d3:ff:ff:ff:ff:ff
0x33
51
53:ff:ff:ff:ff:ff
0x34
52
73:ff:ff:ff:ff:ff
0x35
53
23:ff:ff:ff:ff:ff
0x36
54
13:ff:ff:ff:ff:ff
0x37
55
3d:ff:ff:ff:ff:ff
0x38
56
0d:ff:ff:ff:ff:ff
0x39
57
5d:ff:ff:ff:ff:ff
0x3a
58
7d:ff:ff:ff:ff:ff
0x3b
59
fd:ff:ff:ff:ff:ff
0x3c
60
dd:ff:ff:ff:ff:ff
0x3d
61
9d:ff:ff:ff:ff:ff
0x3e
62
bd:ff:ff:ff:ff:ff
0x3f
63
Table 14-44. Destination Address to 6-Bit Hash (continued)
48-bit DA
6-bit hash (in hex)
hash decimal value

<!-- Slide number: 497 -->
# Initialization Sequence

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-43
Pause frame detection is performed by the receiver and microcontroller modules. The microcontroller runs an address recognition subroutine 
to detect the specified pause frame destination address, while the receiver detects the type and opcode pause frame fields. On detection of a 
pause frame, graceful transmit stop is asserted by the FEC internally. When transmission has paused, the GRA (Graceful Stop complete) 
interrupt is asserted and the pause timer begins to increment. Note that the pause timer makes use of the transmit backoff timer hardware which 
is used for tracking the appropriate collision backoff time in half-duplex mode. The pause timer increments once every slot time until 
PAUSE_DURATION slot times have expired. On PAUSE_DURATION expiration, graceful transmit stop is deasserted allowing MAC data 
frame transmission to resume. Note that the receive flow control pause (X_CNTRL.RFC_PAUSE) status bit is asserted while the transmitter 
is paused due to reception of a pause frame.
To transmit a pause frame the FEC must operate in full-duplex mode and the user must assert flow control pause (X_CNTRL.TFC_PAUSE). 
On assertion of transmit flow control pause (X_CNTRL.TFC_PAUSE) the transmitter asserts graceful transmit stop internally. When the 
transmission of data frames stops the GRA (Graceful Stop complete) interrupt asserts. Following GRA assertion the Pause frame is 
transmitted. On completion of pause frame transmission flow control pause (X_CNTRL.TFC_PAUSE) and graceful transmit stop are 
deasserted internally. 
During pause frame transmission the transmit hardware places data into the transmit data stream from the registers shown in the table below.
The user must specify the desired pause duration in the OP_PAUSE register.
Note that when the transmitter is paused due to receiver/microcontroller pause frame detection, transmit flow control pause 
(X_CNTRL.TFC_PAUSE) still may be asserted and will cause the transmission of a single pause frame. In this case the GRA interrupt will 
not be asserted.
14.9.8
Inter-Packet Gap Time
The minimum inter packet gap time for back-to-back transmission is 96 bit times. After completing a transmission or after the backoff 
algorithm completes the transmitter waits for carrier sense to be negated before starting its 96 bit time IPG counter. Frame transmission may 
begin 96 bit times after carrier sense is negated if it stays negated for at least 60 bit times. If carrier sense asserts during the last 36 bit times 
it will be ignored and a collision will occur.
The receiver receives back-to-back frames with a minimum spacing of at least 28 bit times. If an inter-packet gap between receive frames is 
less than 28 bit times the following frame may be discarded by the receiver.
14.9.9
Collision Handling
If a collision occurs during frame transmission the ethernet controller will continue the transmission for at least 32 bit times, transmitting a 
JAM pattern consisting of 32 1’s. If the collision occurs during the preamble sequence the JAM pattern will be sent after the end of the 
preamble sequence.
Table 14-45. PAUSE Frame Field Specification
48-bit destination address
0180_c200_0001 or Physical ADDRESS
48-bit Source Address
any
16-bit type
8808
16-bit opcode
0001
16-bit PAUSE duration
0000 to ffff
Table 14-46. Transmit Pause Frame Registers
PAUSE FRame fields
FEC register 
Register Contents
48-bit destination address
{FDXFC_DA1[0:31], FDXFC_DA2[0:15]}
0180_c200_0001 
48-bit Source Address
{PADDR1[0:31], PADDR2[0:15]}
physical address
16-bit type
PADDR2[16:31]
8808
16-bit opcode
OP_PAUSE[0:15]
0001
16-bit PAUSE duration
OP_PAUSE[16:31]
0000 to ffff

<!-- Slide number: 498 -->
# MPC5200 Users Guide, Rev. 3.1

14-44
Freescale Semiconductor
Initialization Sequence
If a collision occurs within 64 byte times the retry process is initiated. The transmitter waits a random number of slot times. A slot time is 512 
bit times. If a collision occurs after 64 byte times no retransmission is performed and the end of frame buffer is closed with an LC error 
indication. 
14.9.10
Internal and External Loopback
Both internal and external loopback are supported by the ethernet controller. In loopback mode both of the FIFOs are used and the FEC 
actually operates in a full-duplex fashion. Both internal and external loopback are configured using combinations of the LOOP and DRT bits 
in the R_CNTRL register and the FDEN bit in the X_CNTRL register.
For both internal and external loopback set FDEN = 1.
For internal loopback set LOOP = 1 and DRT = 0. TX_EN and TX_ER will not assert during internal loopback. During internal loopback the 
transmit/receive data rate is higher than in normal operation because the internal system clock is used by the transmit and receive blocks 
instead of the clocks from the external transceiver. This will cause an increase in the required system bus bandwidth for transmit and receive 
data being transferred to/from external memory. It may be necessary to pace the frames on the transmit side and/or limit the size of the frames 
to prevent transmit FIFO underrun and receive FIFO overflow.
For external loopback set LOOP = 0, DRT = 0 and configure the external transceiver for loopback.
14.9.11
Ethernet Error-Handling Procedure
The ethernet controller reports frame reception and transmission error conditions using the FEC BDs (receive), the IEVENT register and the 
MIB block counters.
14.9.11.1
Transmission Errors
Transmitter Underrun
—
If this error occurs the FEC sends 32 bits that ensure a CRC error and stops transmitting. All remaining buffers for that frame 
are then flushed and closed. The UN bit is set in the X_STATUS register. The FEC will then continue to the next transmit buffer 
descriptor and begin transmitting the next frame.
—
The XFIFO_UN interrupt will be asserted if enabled in the IMASK register.
Carrier Sense Lost During Frame Transmission
—
When this error occurs and no collision is detected in the frame the FEC sets the CSL bit in X_STATUS register. The frame is 
transmitted normally. No retries are performed as a result of this error. 
—
No interrupt is generated as a result of this error.
Retransmission Attempts Limit Expired
—
When this error occurs the FEC terminates transmission. All remaining buffers for that frame are then flushed and closed and 
the RL bit is set in the X_STATUS register. The FEC will then continue to the next transmit buffer descriptor and begin 
transmitting the next frame.
—
The COL_RETRY_LIM interrupt will be asserted if enabled in the IMASK register.
Late Collision
—
When a collision occurs after the slot time (512 bits starting at the Preamble), the FEC terminates transmission. All remaining 
buffers for that frame are then flushed and closed and the LC bit is set in the X_STATUS register. The FEC will then continue 
to the next transmit buffer descriptor and begin transmitting the next frame.
—
The LATE_COL interrupt will be asserted if enabled in the IMASK register.
Heartbeat
—
Some transceivers have a self-test feature called “heartbeat” or “signal quality error.” To signify a good self-test the transceiver 
indicates a collision to the FEC within 20 clocks after completion of a frame transmitted by the Ethernet controller. This 
indication of a collision does not imply a real collision error on the network but is rather an indication that the transceiver still 
seems to be functioning properly. This is called the heartbeat condition. 
—
If the HBC bit is set in the X_CNTRL register and the heartbeat condition is not detected by the FEC after a frame transmission 
a heartbeat error occurs. When this error occurs the FEC closes the buffer, sets the HB bit in the X_STATUS register and 
generates the HBERR interrupt if it is enabled.
14.9.11.2
Reception Errors
Overrun Error
—
If the receive block has data to put into the receive FIFO and the receive FIFO is full, the FEC sets the OV bit in the receive 
status word. All subsequent data in the frame will be discarded and subsequent frames may also be discarded until the receive 
FIFO is serviced by the DMA and space is made available. At this point the receive frame/status word is written into the FIFO 
with the OV bit isset. This frame must be discarded by the driver.

<!-- Slide number: 499 -->
# Initialization Sequence

MPC5200 Users Guide, Rev. 3.1
Freescale Semiconductor
14-45
Non-Octet Error (Dribbling Bits) 
—
The Ethernet controller handles up to seven dribbling bits when the receive frame terminates nonoctet aligned and it checks 
the CRC of the frame on the last octet boundary. If there is a CRC error, then the frame nonoctet aligned (NO) error is reported 
in the Receive Frame Status Word . If there is no CRC error, then no error is reported.
CRC Error 
—
When a CRC error occurs with no dribble bits, the FEC closes the buffer and sets the CR bit in the Receive Frame Status Word. 
CRC checking cannot be disabled, but the CRC error can be ignored if checking is not required.
Frame Length Violation
—
When the receive frame length exceeds MAX_FL bytes the BABR interrupt will be generated and the LG bit in the end of 
frame Receive Frame Status Word will be set. The frame is not truncated (truncation occurs if the frame length exceeds 2047 
bytes).
Truncation
—
When the receive frame length exceeds 2047 bytes the frame is truncated and the TR bit is set in the receive BD.

<!-- Slide number: 500 -->
## Chapter 14 — Fast Ethernet Controller (FEC): Notes Page

Page 500 is a blank "Notes" page (page 14-46) at the end of Chapter 14 (Fast Ethernet Controller). No register or technical content.

### Context: Chapter 14 FEC — Key information from adjacent pages

#### 14.1 FEC Overview (from page 455)

The FEC is an Ethernet MAC with two 1 KByte FIFOs controlled by processor and BestComm DMA:
- Supports 10/100 Mbps Ethernet/IEEE 802.3
- Internal blocks: CSR (control/status), RISC controller, FIFO controller, MII, Tx/Rx MAC, MIB counters
- Tx FIFO: CommBus → Tx FIFO → PHY
- Rx FIFO: PHY → Rx block → pulled by BestComm (interrupt-driven)
- MII interface: MDC (clock) + MDIO (bidirectional) for PHY management
- Supports 10/100 Mbps MII and 10 Mbps 7-Wire interfaces

#### 14.9 FEC Error Handling (from pages 497–499)

**PAUSE Frame Format (Table 14-45):**

| Field | Value |
|-------|-------|
| 48-bit destination | 0180_c200_0001 or Physical ADDRESS |
| 48-bit source | any |
| 16-bit type | 0x8808 |
| 16-bit opcode | 0x0001 |
| 16-bit PAUSE duration | 0x0000–0xFFFF |

**Transmit Pause Frame Registers (Table 14-46):**

| PAUSE Frame Field | FEC Register | Value |
|------------------|-------------|-------|
| 48-bit destination | {FDXFC_DA1[0:31], FDXFC_DA2[0:15]} | 0180_c200_0001 |
| 48-bit source | {PADDR1[0:31], PADDR2[0:15]} | physical address |
| 16-bit type | PADDR2[16:31] | 0x8808 |
| 16-bit opcode | OP_PAUSE[0:15] | 0x0001 |
| 16-bit duration | OP_PAUSE[16:31] | 0x0000–0xFFFF |

**Transmission Errors:**
- **Underrun:** FEC sends 32-bit CRC-error pattern, flushes buffers, sets `X_STATUS.UN`, asserts `XFIFO_UN` interrupt if enabled
- **Carrier Sense Lost:** Sets `X_STATUS.CSL`, no retry, no interrupt
- **Retry Limit Expired:** Terminates frame, sets `X_STATUS.RL`, asserts `COL_RETRY_LIM` interrupt
- **Late Collision** (after 512-bit slot time): Terminates frame, sets `X_STATUS.LC`, asserts `LATE_COL` interrupt
- **Heartbeat Error:** Sets `X_STATUS.HB`, asserts `HBERR` interrupt if `X_CNTRL.HBC` set

**Reception Errors:**
- **Overrun:** Sets `OV` bit in receive status word; frame must be discarded by driver
- **Non-Octet (Dribbling bits):** Up to 7 dribble bits handled; `NO` error set only if CRC fails
- **CRC Error:** Sets `CR` bit in Receive Frame Status Word
- **Frame Length Violation** (>MAX_FL): Asserts `BABR` interrupt, sets `LG` bit; frame not truncated unless >2047 bytes
- **Truncation** (>2047 bytes): Frame truncated, `TR` bit set in receive BD

**Inter-Packet Gap:** Minimum 96 bit times for back-to-back Tx; receiver accepts minimum 28 bit times spacing.

**Loopback Modes:**
- Internal loopback: `R_CNTRL.LOOP=1`, `R_CNTRL.DRT=0`, `X_CNTRL.FDEN=1`. TX_EN/TX_ER not asserted.
- External loopback: `R_CNTRL.LOOP=0`, `R_CNTRL.DRT=0`, `X_CNTRL.FDEN=1`, configure PHY for loopback.

