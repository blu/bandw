# bandw
A low-level bandwidth tester for Ethernet LANs.

Description
-----------

Ever wondered if you could gain an extra bit of bandwidth or trim some latency by bypassing IP protocol on a LAN? This tool can give you a hint. bandw works across two machines located on the same LAN. Moreover, that LAN should be truly quiet - any chatter on it will disrupt the measurement and result in an error. That means you cannot even have a ssh connection to the test interfaces of either machines (but you can to other interfaces, if available).

Some sample measurements (only the payload of the frame taken into account, i.e. 1500 bytes/frame):

Gigabit Ethernet, dedicated interfaces on two desktops:
	bandwidth 121579830 bytes/s

54Mbit-hotspot WiFi Ethernet, quiet interfaces on a desktop and a notebook:
	bandwidth 1728900 bytes/s

Usage
-----

First, launch the responder (in this case on interface eth1; substitute dummy target MAC for that of the transmitter):
$ sudo ./bandw -interface eth1 -target 0x01:0x02:0x03:0x04:0x05:0x06 -packetcount 2048

Then run the transmitter (in this case on interface eth2; substitute dummy target MAC for that of the responder):
$ sudo ./bandw -interface eth2 -target 0x06:0x05:0x04:0x03:0x02:0x01 -packetcount 2048 -transmitter

If you're wondering why the responder needs to know the transmitter's address - I didn't bother to implement address readback for the responder.

As a rule of thumb, if the connection is not quiet enough, try reducing the packet count and thus increasing the chances to get a quiet window.
