#ifndef __MADCDIS_CHANNEL_H
#define __MADCDIS_CHANNEL_H 1

#include "madcdis/datachannel.h"

#include <string>

namespace madc {

// madc::channel — one URI-addressed byte channel with line helpers, the
// script-facing convenience surface over the DataChannel registry (and the
// embedding host's convenience wrapper — same class, cpp-first).
//
// LAYOUT CONTRACT: a single void *impl_ member, append-only. The embedded
// header <ns_madc> declares this class declaration-only and scripts resolve
// its methods mangled-direct against libmadc — the two declarations must
// stay layout- and signature-identical (SysInfo precedent).
//
// Modes: "r" read, "w" write, "rw" read+write (default), "a" append.
// readline() strips the trailing newline (and a preceding '\r' if present)
// and returns the final unterminated tail; it returns false only at EOF.
class channel
{
public:
	explicit channel(const char *uri);
	channel(const char *uri, const char *mode);
	~channel();

	bool ok() const;
	const char *last_error() const;

	long read(void *buffer, long capacity);
	bool readline(std::string &out);
	bool readall(std::string &out);
	bool write(const char *text);
	bool write(const char *buffer, long size);
	bool write(std::string &text);
	bool close_write();
	void close();

private:
	channel(const channel &);
	channel &operator=(const channel &);

	void *impl_;
};

} // namespace madc

#endif // __MADCDIS_CHANNEL_H
