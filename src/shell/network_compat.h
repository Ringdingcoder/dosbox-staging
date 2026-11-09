// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_SHELL_NETWORK_COMPAT_H
#define DOSBOX_SHELL_NETWORK_COMPAT_H

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#endif

// shell.cpp uses the GNU gethostbyname_r signature. Provide that signature on
// Windows and macOS using the portable getaddrinfo API instead.
#if defined(WIN32) || defined(MACOSX)
static inline int portable_gethostbyname_r(const char* name,
                                           struct hostent* host,
                                           char* buffer,
                                           size_t buffer_size,
                                           struct hostent** result,
                                           int* host_error)
{
	if (!name || !host || !buffer || !result) {
		return EINVAL;
	}

	struct addrinfo hints = {};
	hints.ai_family = AF_INET;
	struct addrinfo* addresses = nullptr;
	const int lookup_result = getaddrinfo(name, nullptr, &hints, &addresses);

	struct in_addr address = {};
	if (lookup_result == 0) {
		for (auto* entry = addresses; entry; entry = entry->ai_next) {
			if (entry->ai_family == AF_INET && entry->ai_addr) {
				address = reinterpret_cast<struct sockaddr_in*>(entry->ai_addr)->sin_addr;
				break;
			}
		}
		freeaddrinfo(addresses);
	}

	// The caller starts with a 1024-byte buffer. Keep the hostent's pointer
	// arrays and data in that buffer, as required by the reentrant API.
	const auto pointer_bytes = 4 * sizeof(char*);
	const auto required_size = pointer_bytes + sizeof(address) + std::strlen(name) + 1;
	if (buffer_size < required_size) {
		return ERANGE;
	}

	auto** aliases = reinterpret_cast<char**>(buffer);
	auto** address_list = aliases + 1;
	auto* address_data = reinterpret_cast<char*>(address_list + 2);
	auto* name_data = address_data + sizeof(address);
	std::memcpy(address_data, &address, sizeof(address));
	std::memcpy(name_data, name, std::strlen(name) + 1);

	aliases[0] = nullptr;
	address_list[0] = address_data;
	address_list[1] = nullptr;
	host->h_name = name_data;
	host->h_aliases = aliases;
	host->h_addrtype = AF_INET;
	host->h_length = sizeof(address);
	host->h_addr_list = address_list;
	*result = host;
	if (host_error) {
		*host_error = (lookup_result == 0) ? 0 : HOST_NOT_FOUND;
	}
	return 0;
}
#define gethostbyname_r portable_gethostbyname_r

// reallocarray is not available in all supported C runtimes.
static inline void* portable_reallocarray(void* pointer, size_t count, size_t size)
{
	if (size && count > SIZE_MAX / size) {
		errno = ENOMEM;
		return nullptr;
	}
	return std::realloc(pointer, count * size);
}
#define reallocarray portable_reallocarray
#endif

#endif // DOSBOX_SHELL_NETWORK_COMPAT_H
