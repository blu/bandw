//
// bandw - a rudimentary bandwidth-metering tool for eth networks (half-duplex)
//

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <memory.h>
#include <unistd.h>
#include <assert.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <errno.h>

// raw ethernet frames
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>

static const char argInterface[]   = "-interface";
static const char argTarget[]      = "-target";
static const char argPacketCount[] = "-packetcount";
static const char argTransmitter[] = "-transmitter";

static const uint32_t magic = 0x32100123;

// eth frame geometry, sans preamble and FCS/CRC
static const size_t frame_min_size = ETH_ZLEN;      // minimal frame size (14 octets header + 46 octets payload)
static const size_t frame_max_size = ETH_FRAME_LEN; // full frame size (14 octets header + 1500 octets payload)
static const size_t packet_size = ETH_DATA_LEN;     // payload in the full frame

class non_copyable
{
	non_copyable(const non_copyable&) {}
	non_copyable& operator =(const non_copyable&) { return *this; }

public:
	non_copyable() {}
};

template < typename T, class FTOR_T >
class scoped : public non_copyable, private FTOR_T {
	T m;

public:
	scoped(T arg)
	: m(arg) {
	}

	~scoped() {
		FTOR_T::operator()(m);
	}

	operator T () const {
		return m;
	}
};

struct generic_free {
	void operator()(void* const ptr) const {
		if (0 != ptr)
			free(ptr);
	}
};

struct close_file_descriptor {
	void operator ()(const int fd) const {
		if (0 <= fd)
			close(fd);
	}
};

static uint64_t timer_nsec() {

	timespec t;
	clock_gettime(CLOCK_MONOTONIC_RAW, &t);

	return t.tv_sec * uint64_t(1e9) + t.tv_nsec;
}

// simultaneously init ethernet frame header and socket address structure
// based on http://aschauf.landshut.org/fh/linux/udp_vs_raw/ch01s03.html
static bool init_ethhdr_and_saddr(
	const int fd,                 // file descriptor
	const char* const iface_name, // iface name, cstr
	const size_t iface_namelen,   // iface name length, shorter than IFNAMSIZ
	const uint8_t (& target)[8],  // target mac address, last two octets unused
	ethhdr& ethhead,              // output: ethernet frame header
	sockaddr_ll& saddr) {         // output: socket address structure

	assert(IFNAMSIZ	> iface_namelen);

	// pretend we're an established protocol type, lest we get dropped by some self-important filter
	const uint16_t proto = ETH_P_IP;

	// simultaneously fill-in socket address and eth frame header
	memset(&saddr, 0, sizeof(saddr));

	saddr.sll_family   = AF_PACKET;
	saddr.sll_protocol = htons(proto);
	saddr.sll_hatype   = ARPHRD_ETHER;
	saddr.sll_pkttype  = PACKET_OTHERHOST;
	saddr.sll_halen    = ETH_ALEN;

	for (size_t i = 0; i < ETH_ALEN; ++i) {
		saddr.sll_addr[i] = target[i];
		ethhead.h_dest[i] = target[i];
	}

	ifreq ifr;
	memcpy(ifr.ifr_name, iface_name, iface_namelen);
	ifr.ifr_name[iface_namelen] = '\0';

	// ioctl about iface index
	if (-1 == ioctl(fd, SIOCGIFINDEX, &ifr)) {
		fprintf(stderr, "error: cannot obtain iface name (errno %s)\n", strerror(errno));
		return false;
	}

	saddr.sll_ifindex  = ifr.ifr_ifindex;

	// ioctl about iface hwaddr
	if (-1 == ioctl(fd, SIOCGIFHWADDR, &ifr)) {
		fprintf(stderr, "error: cannot obtain iface hw addr (errno %s)\n", strerror(errno));
		return false;
	}

	for (size_t i = 0; i < ETH_ALEN; ++i)
		ethhead.h_source[i] = ifr.ifr_hwaddr.sa_data[i];

	ethhead.h_proto = htons(proto);

	return true;
}

int main(
	int argc,
	char** argv) {

	uint32_t iface_nameidx    = 0;
	uint32_t iface_namelen    = 0;
	uint8_t target[8]         = { 0 };
	uint32_t packet_count     = 0;
	uint32_t flags            = 0;
	enum {
		flag_transmitter = 1,
		flag_target      = 2
	};

	// get command line arguments
	bool cmd_err = false;

	for (uint32_t i = 1; i < argc && !cmd_err; ++i) {
		cmd_err = true;

		if (!strcmp(argv[i], argInterface)) {
			if (++i < argc && !iface_nameidx) {
				iface_namelen = strlen(argv[i]);

				if (IFNAMSIZ > iface_namelen) {
					iface_nameidx = i;
					cmd_err = false;
				}
			}
			continue;
		}

		if (!strcmp(argv[i], argTarget)) {
			if (++i < argc && !(flags & flag_target)) {
				if (6 == sscanf(argv[i], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
					&target[0],
					&target[1],
					&target[2],
					&target[3],
					&target[4],
					&target[5]))
				{
					flags |= flag_target;
					cmd_err = false;
				}
			}
			continue;
		}

		if (!strcmp(argv[i], argPacketCount)) {
			if (++i < argc && !packet_count) {
				uint32_t count = 0;

				if (1 == sscanf(argv[i], "%u", &count) && count) {
					packet_count = count;
					cmd_err = false;
				}
			}
			continue;
		}

		if (!strcmp(argv[i], argTransmitter)) {
			flags |= flag_transmitter;
			cmd_err = false;
			continue;
		}
	}

	if (cmd_err || !iface_nameidx || !(flags & flag_target) || !packet_count) {
		printf("usage: %s %s iface %s target_mac %s N [%s]\n",
				argv[0],
				argInterface,
				argTarget,
				argPacketCount,
				argTransmitter);
		return -1;
	}

	// room for two ethernet frames, plase
	const scoped< uint8_t*, generic_free > frame(
		reinterpret_cast< uint8_t* >(malloc(frame_max_size * 2)));

	if (0 == frame) {
		fprintf(stderr, "error: cannot allocate buffer\n");
		return -1;
	}

	// frame0 - outgoing, frame1 - incoming
	uint8_t* const frame0 = frame;
	uint8_t* const frame1 = frame + frame_max_size;
	uint8_t* const payload0 = frame0 + ETH_HLEN;
	uint8_t* const payload1 = frame1 + ETH_HLEN;

	// get an ethernet socket
	const scoped< int, close_file_descriptor > fd(
		socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)));

	if (0 > fd) {
		fprintf(stderr, "error: cannot create socket\n");
		return -1;
	}

	sockaddr_ll saddr;

	if (!init_ethhdr_and_saddr(fd, argv[iface_nameidx], iface_namelen, target, *reinterpret_cast< ethhdr* >(frame0), saddr)) {
		return -1;
	}

	// bind socket to specified iface (for the receiving part)
	if (0 > bind(fd, reinterpret_cast< sockaddr* >(&saddr), sizeof(saddr))) {
		fprintf(stderr, "error: cannot bind to iface\n");
		return -1;
	}

	if (flags & flag_transmitter) { // are we a transmitter?

		printf("transmitter at interface %s\n", argv[iface_nameidx]);

		const uint64_t t0 = timer_nsec();

		for (uint32_t i = 0; i < packet_count; ++i) {
			// this communication is intended for same-endian peers -- no need to go through network endianness
			reinterpret_cast< uint32_t* >(payload0)[0] = magic;
			reinterpret_cast< uint32_t* >(payload0)[1] = i;

			const ssize_t sent = sendto(fd, frame0, frame_max_size, 0, reinterpret_cast< sockaddr* >(&saddr), sizeof(saddr));

			if (frame_max_size != sent) {
				fprintf(stderr, "error: sendto() failed to send requested byte count\n");
				return -1;
			}
		}

		for (uint32_t i = 0; i < packet_count; ++i) {
			const ssize_t recv = recvfrom(fd, frame1, frame_max_size, 0, 0, 0);

			if (frame_max_size != recv) {
				fprintf(stderr, "error: recvfrom() got unexpected byte count\n");
				return -1;
			}

			if (reinterpret_cast< uint32_t* >(payload1)[0] != magic ||
				reinterpret_cast< uint32_t* >(payload1)[1] != i) {

				fprintf(stderr, "error: bad response package %u\n", i);
				return -1;
			} 
		}

		const uint64_t dt = timer_nsec() - t0;

		if (0 != dt) {
			const double transcieved = double(packet_size) * double(packet_count) * 2.0;
			const double s = double(dt) * 1e-9;
			const double bandwidth = transcieved / s;

			printf("elapsed time %f s\ntransceived %.0f bytes\nbandwidth %f bytes/s\n",
					s,
					transcieved,
					bandwidth);
		}
		else
			printf("session failed\n");
	}
	else { // we are a responder

		printf("responder at interface %s\n", argv[iface_nameidx]);

		for (uint32_t i = 0; i < packet_count; ++i) {
			const ssize_t recv = recvfrom(fd, frame1, frame_max_size, 0, 0, 0);

			if (frame_max_size != recv) {
				fprintf(stderr, "error: recvfrom() got unexpected byte count\n");
				return -1;
			}

			// this communication is intended for same-endian peers -- no need to go through network endianness
			if (reinterpret_cast< uint32_t* >(payload1)[0] != magic ||
				reinterpret_cast< uint32_t* >(payload1)[1] != i) {

				fprintf(stderr, "error: bad request package %u\n", i);
				return -1;
			} 
		}

		for (uint32_t i = 0; i < packet_count; ++i) {
			reinterpret_cast< uint32_t* >(payload0)[0] = magic;
			reinterpret_cast< uint32_t* >(payload0)[1] = i;

			const ssize_t sent = sendto(fd, frame0, frame_max_size, 0, reinterpret_cast< sockaddr* >(&saddr), sizeof(saddr));

			if (frame_max_size != sent) {
				fprintf(stderr, "error: sendto() failed to send requested byte count\n");
				return -1;
			}
		}
	}

	return 0;
}
