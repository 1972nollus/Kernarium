// SPDX-License-Identifier: GPL-2.0-or-later
// What a product's kernel configuration gives its guest, for the settings the
// Android CTS and the desktop disagree on. usage: kernel_config_probe
// [linux|android], linux by default.
//   both:    IPv4 multicast (/proc/net/igmp, a group join, a looped datagram),
//            no RDS socket family, no listener on TCP port 16385, and the
//            stack protector and hardened usercopy in /proc/config.gz
//   linux:   SysV IPC works, the NFS client and SCTP are present, and
//            /sys/power has no wake_lock
//   android: no SysV IPC (ENOSYS, no /proc/sysvipc), no NFS client, no SCTP,
//            and /sys/power/wake_lock takes and releases a wakelock
// Prints one line per check. The exit status is the failure count.
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void report(const char *name, bool ok, const std::string &detail)
{
	std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
	if (!ok)
		++failures;
}

std::string slurp(const char *path)
{
	std::ifstream in(path);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}

std::string errno_text(int e)
{
	return std::to_string(e) + " " + std::strerror(e);
}

// A bare test shell has no network. Bring loopback up with its address, as a
// root shell can, so a guest without another interface can still join a group
// on it. A guest that has the address up already is left as it is.
void prepare_loopback()
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return;
	ifreq request{};
	std::strncpy(request.ifr_name, "lo", sizeof request.ifr_name - 1);
	if (ioctl(fd, SIOCGIFFLAGS, &request) == 0 && (request.ifr_flags & IFF_UP) == 0) {
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		std::memcpy(&request.ifr_addr, &address, sizeof address);
		ioctl(fd, SIOCSIFADDR, &request);
		address.sin_addr.s_addr = htonl(0xff000000U);
		std::memcpy(&request.ifr_netmask, &address, sizeof address);
		ioctl(fd, SIOCSIFNETMASK, &request);
		ioctl(fd, SIOCGIFFLAGS, &request);
		request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP | IFF_RUNNING);
		ioctl(fd, SIOCSIFFLAGS, &request);
	}
	close(fd);
}

// The chosen interface: first up, non-loopback, multicast-capable IPv4
// interface; else loopback.
bool pick_interface(in_addr &address, std::string &name)
{
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0)
		return false;
	bool found = false;
	for (ifaddrs *i = list; i != nullptr && !found; i = i->ifa_next) {
		if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET)
			continue;
		unsigned flags = i->ifa_flags;
		if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0 ||
		    (flags & IFF_MULTICAST) == 0)
			continue;
		address = reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr;
		name = i->ifa_name;
		found = true;
	}
	if (!found) {
		address.s_addr = htonl(INADDR_LOOPBACK);
		name = "lo";
	}
	freeifaddrs(list);
	return true;
}

void check_multicast()
{
	std::string igmp = slurp("/proc/net/igmp");
	report("igmp-proc", access("/proc/net/igmp", F_OK) == 0 && !igmp.empty(),
	       "/proc/net/igmp is " + std::string(igmp.empty() ? "missing" : "present"));

	prepare_loopback();
	in_addr local{};
	std::string ifname;
	if (!pick_interface(local, ifname)) {
		report("multicast-iface", false, "getifaddrs failed");
		return;
	}
	char local_text[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &local, local_text, sizeof local_text);
	std::printf("INFO interface %s %s\n", ifname.c_str(), local_text);

	int rx = socket(AF_INET, SOCK_DGRAM, 0);
	int tx = socket(AF_INET, SOCK_DGRAM, 0);
	if (rx < 0 || tx < 0) {
		report("multicast-socket", false, errno_text(errno));
		return;
	}
	int yes = 1;
	setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
	sockaddr_in bind_address{};
	bind_address.sin_family = AF_INET;
	bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_address.sin_port = htons(40123);
	if (bind(rx, reinterpret_cast<sockaddr *>(&bind_address), sizeof bind_address) != 0) {
		report("multicast-bind", false, errno_text(errno));
		return;
	}
	ip_mreq membership{};
	inet_pton(AF_INET, "239.255.42.99", &membership.imr_multiaddr);
	membership.imr_interface = local;
	int joined = setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof membership);
	report("multicast-join", joined == 0,
	       joined == 0 ? "joined 239.255.42.99" : errno_text(errno));
	if (joined != 0)
		return;

	// The group shows in /proc/net/igmp as the address in network byte order,
	// printed as a host-order hex word: 239.255.42.99 is 632AFFEF on a
	// little-endian host.
	igmp = slurp("/proc/net/igmp");
	report("multicast-listed", igmp.find("632AFFEF") != std::string::npos,
	       "group 239.255.42.99 in /proc/net/igmp");

	setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof local);
	unsigned char loop = 1;
	setsockopt(tx, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
	unsigned char ttl = 1;
	setsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
	sockaddr_in group{};
	group.sin_family = AF_INET;
	group.sin_addr = membership.imr_multiaddr;
	group.sin_port = htons(40123);
	const char payload[] = "kconfig-multicast";
	ssize_t sent = sendto(tx, payload, sizeof payload, 0,
			      reinterpret_cast<sockaddr *>(&group), sizeof group);
	if (sent != static_cast<ssize_t>(sizeof payload)) {
		report("multicast-send", false, errno_text(errno));
		return;
	}
	timeval timeout{3, 0};
	setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	char buffer[64] = {};
	ssize_t got = recv(rx, buffer, sizeof buffer, 0);
	report("multicast-loop", got == static_cast<ssize_t>(sizeof payload) &&
					 std::strcmp(buffer, payload) == 0,
	       got < 0 ? errno_text(errno) : std::string("received ") + buffer);
	setsockopt(rx, IPPROTO_IP, IP_DROP_MEMBERSHIP, &membership, sizeof membership);
	close(rx);
	close(tx);
}

// A TCP socket in the LISTEN state (0A) on port 16385 (0x4001), in either
// family's table.
void check_no_rds_listener()
{
	bool found = false;
	for (const char *path : {"/proc/net/tcp", "/proc/net/tcp6"}) {
		std::istringstream lines(slurp(path));
		std::string line;
		while (std::getline(lines, line)) {
			std::istringstream fields(line);
			std::string index, local, remote, state;
			fields >> index >> local >> remote >> state;
			size_t colon = local.rfind(':');
			if (colon == std::string::npos)
				continue;
			if (state == "0A" && local.substr(colon + 1) == "4001") {
				found = true;
				std::printf("INFO listener %s %s\n", path, local.c_str());
			}
		}
	}
	report("no-port-16385-listener", !found, found ? "listening" : "none");
}

// Whether /proc/net/protocols lists a protocol: a policy can refuse a socket
// of a family the kernel has, so the error alone does not show its absence.
bool protocol_registered(const char *name)
{
	std::istringstream lines(slurp("/proc/net/protocols"));
	std::string line;
	while (std::getline(lines, line)) {
		std::istringstream fields(line);
		std::string first;
		fields >> first;
		if (first == name)
			return true;
	}
	return false;
}

void check_rds()
{
	int fd = socket(AF_RDS, SOCK_SEQPACKET, 0);
	int e = errno;
	if (fd >= 0)
		close(fd);
	bool registered = protocol_registered("RDS");
	report("no-rds", fd < 0 && !registered && e != 0,
	       std::string(registered ? "RDS registered, " : "RDS not registered, ") +
		       (fd >= 0 ? std::string("AF_RDS socket created") : errno_text(e)));
}

bool sysv_ok(long result, int &e)
{
	e = errno;
	return result >= 0;
}

void check_sysv(bool wanted)
{
	int e = 0;
	long msg = syscall(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
	bool msg_ok = sysv_ok(msg, e);
	int msg_errno = e;
	if (msg_ok)
		syscall(SYS_msgctl, static_cast<int>(msg), IPC_RMID, nullptr);
	long sem = syscall(SYS_semget, IPC_PRIVATE, 1, IPC_CREAT | 0600);
	bool sem_ok = sysv_ok(sem, e);
	int sem_errno = e;
	if (sem_ok)
		syscall(SYS_semctl, static_cast<int>(sem), 0, IPC_RMID, 0);
	long shm = syscall(SYS_shmget, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	bool shm_ok = sysv_ok(shm, e);
	int shm_errno = e;
	if (shm_ok)
		syscall(SYS_shmctl, static_cast<int>(shm), IPC_RMID, nullptr);
	bool proc = access("/proc/sysvipc", F_OK) == 0;
	std::string detail = "msgget " + (msg_ok ? std::string("ok") : errno_text(msg_errno)) +
			     ", semget " + (sem_ok ? std::string("ok") : errno_text(sem_errno)) +
			     ", shmget " + (shm_ok ? std::string("ok") : errno_text(shm_errno)) +
			     ", /proc/sysvipc " + (proc ? "present" : "absent");
	if (wanted) {
		report("sysv-ipc", msg_ok && sem_ok && shm_ok && proc, detail);
	} else {
		report("no-sysv-ipc", !msg_ok && msg_errno == ENOSYS && !sem_ok &&
					      sem_errno == ENOSYS && !shm_ok &&
					      shm_errno == ENOSYS && !proc,
		       detail);
	}
}

void check_nfs(bool wanted)
{
	std::string filesystems = slurp("/proc/filesystems");
	bool nfs = filesystems.find("\tnfs\n") != std::string::npos;
	bool nfs4 = filesystems.find("\tnfs4\n") != std::string::npos;
	std::string detail = std::string("nfs ") + (nfs ? "present" : "absent") + ", nfs4 " +
			     (nfs4 ? "present" : "absent");
	if (wanted)
		report("nfs-client", nfs && nfs4, detail);
	else
		report("no-nfs-client", !nfs && !nfs4, detail);
}

void check_sctp(bool wanted)
{
	int fd = socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP);
	int e = errno;
	if (fd >= 0)
		close(fd);
	bool registered = protocol_registered("SCTP");
	std::string detail = std::string(registered ? "SCTP registered, " : "SCTP not registered, ") +
			     (fd >= 0 ? std::string("socket created") : errno_text(e));
	if (wanted)
		report("sctp", fd >= 0 && registered, detail);
	else
		report("no-sctp", fd < 0 && !registered, detail);
}

bool write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return false;
	ssize_t length = static_cast<ssize_t>(std::strlen(text));
	bool ok = write(fd, text, static_cast<size_t>(length)) == length;
	close(fd);
	return ok;
}

// Android's SystemSuspend ends a suspend that is under way by writing
// "userspace-abort" to /sys/power/wake_lock, and Sxmo suspends a guest on its
// own whenever the file exists. /sys/power/state must exist for the absence
// to mean anything.
void check_wake_lock(bool wanted)
{
	bool power = access("/sys/power/state", F_OK) == 0;
	bool present = access("/sys/power/wake_lock", F_OK) == 0;
	if (!wanted) {
		report("no-wake-lock", power && !present,
		       std::string("/sys/power/state ") + (power ? "present" : "absent") +
			       ", /sys/power/wake_lock " + (present ? "present" : "absent"));
		return;
	}
	const char *name = "kconfig-probe";
	bool taken = present && write_text("/sys/power/wake_lock", name);
	bool listed = taken && slurp("/sys/power/wake_lock").find(name) != std::string::npos;
	bool released = taken && write_text("/sys/power/wake_unlock", name) &&
			slurp("/sys/power/wake_lock").find(name) == std::string::npos;
	report("wake-lock", power && listed && released,
	       std::string("wake_lock ") + (present ? "present" : "absent") + ", taken " +
		       (taken ? "yes" : "no") + ", listed " + (listed ? "yes" : "no") +
		       ", released " + (released ? "yes" : "no"));
}

// The resolved kernel configuration, as /proc/config.gz holds it. zcat is run
// without a shell, which an Android guest does not have at /bin/sh.
std::string kernel_config()
{
	int fds[2];
	if (pipe(fds) != 0)
		return {};
	pid_t child = fork();
	if (child < 0) {
		close(fds[0]);
		close(fds[1]);
		return {};
	}
	if (child == 0) {
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, 2);
		execlp("zcat", "zcat", "/proc/config.gz", static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	std::string text;
	char chunk[4096];
	ssize_t got;
	while ((got = read(fds[0], chunk, sizeof chunk)) > 0)
		text.append(chunk, static_cast<size_t>(got));
	close(fds[0]);
	int status = 0;
	waitpid(child, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? text : std::string();
}

bool config_has(const std::string &config, const std::string &line)
{
	return config.find("\n" + line + "\n") != std::string::npos ||
	       config.compare(0, line.size() + 1, line + "\n") == 0;
}

// The kernel hardening the Android CTS requires: the strong stack protector
// and hardened usercopy.
void check_hardening_config()
{
	std::string config = kernel_config();
	if (config.empty()) {
		report("hardening-config", false, "/proc/config.gz is unreadable");
		return;
	}
	bool protector = config_has(config, "CONFIG_STACKPROTECTOR_STRONG=y");
	bool usercopy = config_has(config, "CONFIG_HARDENED_USERCOPY=y");
	report("hardening-config", protector && usercopy,
	       std::string("strong stack protector ") + (protector ? "on" : "off") +
		       ", hardened usercopy " + (usercopy ? "on" : "off"));
}

} // namespace

int main(int argc, char **argv)
{
	const char *product = argc == 2 ? argv[1] : "linux";
	if (argc > 2 || (std::strcmp(product, "android") != 0 && std::strcmp(product, "linux") != 0)) {
		std::fprintf(stderr, "usage: kernel_config_probe [linux|android]\n");
		return 125;
	}
	bool android = std::strcmp(product, "android") == 0;
	check_multicast();
	check_rds();
	check_no_rds_listener();
	check_sysv(!android);
	check_nfs(!android);
	check_sctp(!android);
	check_wake_lock(android);
	check_hardening_config();
	std::printf("%s: %d failure(s)\n", product, failures);
	return failures;
}
