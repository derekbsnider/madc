#include "madcdis/channel.h"
#include "madc_datachannel_internal.h"
#include "ns_common.h"

#include <cstring>

namespace madc {
namespace {

// The layout contract in madcdis/channel.h pins channel to one void *impl_;
// everything mutable lives here so the class can grow without ABI breaks.
struct ChannelState
{
	std::unique_ptr<DataChannel> channel;
	error last_error;
	std::string pending;
	bool eof = false;
	bool failed = false;
};

ChannelState *state(void *impl)
{
	return static_cast<ChannelState *>(impl);
}

void set_state_error(ChannelState *s, const std::string &message)
{
	s->failed = true;
	detail::set_channel_error(&s->last_error, message);
}

bool parse_mode(const char *mode, ChannelOpenMode &out)
{
	if ( !mode )
		return false;
	if ( std::strcmp(mode, "r") == 0 )
		out = ChannelOpenMode::read;
	else if ( std::strcmp(mode, "w") == 0 )
		out = ChannelOpenMode::write;
	else if ( std::strcmp(mode, "rw") == 0 )
		out = ChannelOpenMode::read_write;
	else if ( std::strcmp(mode, "a") == 0 )
		out = ChannelOpenMode::append;
	else
		return false;
	return true;
}

void open_channel_state(ChannelState *s, const char *uri, const char *mode)
{
	ChannelOpenMode open_mode = ChannelOpenMode::read_write;
	if ( !parse_mode(mode, open_mode) )
	{
		set_state_error(s, std::string("invalid channel mode: ")
				   + (mode ? mode : "(null)"));
		return;
	}
	if ( !uri || !*uri )
	{
		set_state_error(s, "channel requires a URI");
		return;
	}
	s->channel = DataChannelRegistry::instance().open(
		DataSource(uri), open_mode, &s->last_error);
	if ( !s->channel )
		s->failed = true;
}

// Pull one chunk into pending; false only on a read error (EOF sets s->eof).
bool fill_pending(ChannelState *s)
{
	char buffer[4096];
	std::size_t count = 0;
	if ( !s->channel->read(buffer, sizeof(buffer), count, &s->last_error) )
	{
		s->failed = true;
		return false;
	}
	if ( count == 0 )
		s->eof = true;
	else
		s->pending.append(buffer, count);
	return true;
}

} // namespace

channel::channel(const char *uri)
	: impl_(new ChannelState())
{
	open_channel_state(state(impl_), uri, "rw");
}

channel::channel(const char *uri, const char *mode)
	: impl_(new ChannelState())
{
	open_channel_state(state(impl_), uri, mode);
}

channel::~channel()
{
	close();
	delete state(impl_);
}

bool channel::ok() const
{
	ChannelState *s = state(impl_);
	return s->channel && !s->failed;
}

const char *channel::last_error() const
{
	return state(impl_)->last_error.message.c_str();
}

long channel::read(void *buffer, long capacity)
{
	ChannelState *s = state(impl_);
	if ( !buffer || capacity <= 0 )
		return 0;
	if ( !s->pending.empty() )
	{
		std::size_t count = s->pending.size();
		if ( count > static_cast<std::size_t>(capacity) )
			count = static_cast<std::size_t>(capacity);
		std::memcpy(buffer, s->pending.data(), count);
		s->pending.erase(0, count);
		return static_cast<long>(count);
	}
	if ( s->failed )
		return -1;
	if ( !s->channel )
	{
		set_state_error(s, "channel is closed");
		return -1;
	}
	if ( s->eof )
		return 0;
	std::size_t count = 0;
	if ( !s->channel->read(buffer, static_cast<std::size_t>(capacity),
			       count, &s->last_error) )
	{
		s->failed = true;
		return -1;
	}
	if ( count == 0 )
		s->eof = true;
	return static_cast<long>(count);
}

bool channel::readline(std::string &out)
{
	ChannelState *s = state(impl_);
	out.clear();
	if ( s->failed )
		return false;
	for ( ;; )
	{
		std::size_t newline = s->pending.find('\n');
		if ( newline != std::string::npos )
		{
			std::size_t length = newline;
			if ( length && s->pending[length - 1] == '\r' )
				--length;
			out.assign(s->pending, 0, length);
			s->pending.erase(0, newline + 1);
			return true;
		}
		if ( !s->channel || s->eof )
			break;
		if ( !fill_pending(s) )
			return false;
	}
	// EOF with no newline: the unterminated tail is still a line.
	if ( s->pending.empty() )
		return false;
	out.swap(s->pending);
	return true;
}

bool channel::readall(std::string &out)
{
	ChannelState *s = state(impl_);
	out.clear();
	if ( s->failed )
		return false;
	while ( s->channel && !s->eof )
		if ( !fill_pending(s) )
			return false;
	out.swap(s->pending);
	return true;
}

// value-carrier twins (slice V1): delegate to the string implementations —
// one line/payload owner — and retag the carrier as string kind. On a false
// return the carrier is left null (never a stale previous line).
bool channel::readline(value &out)
{
	std::string line;
	bool got = readline(line);
	out = got ? value(line) : value();
	return got;
}

bool channel::readall(value &out)
{
	std::string payload;
	bool got = readall(payload);
	out = got ? value(payload) : value();
	return got;
}

bool channel::write(const char *text)
{
	if ( !text )
		return false;
	return write(text, static_cast<long>(std::strlen(text)));
}

bool channel::write(const char *buffer, long size)
{
	ChannelState *s = state(impl_);
	if ( !s->channel )
	{
		set_state_error(s, "channel is closed");
		return false;
	}
	if ( !buffer || size <= 0 )
		return size <= 0;
	if ( !write_all(*s->channel, buffer, static_cast<std::size_t>(size),
			&s->last_error) )
	{
		s->failed = true;
		return false;
	}
	return true;
}

// Non-const string& matches the eval-family script surface (const-qualified
// script types are still an active front-end track; see <ns_madc>).
bool channel::write(std::string &text)
{
	return write(text.data(), static_cast<long>(text.size()));
}

// value carrier: send the text view (string kind sends its payload
// directly; other scalar kinds render through the one value->text owner).
bool channel::write(value &text)
{
	if ( text.is_string() )
		return write(static_cast<const char *>(text.data()),
			     static_cast<long>(text.size()));
	std::string rendered;
	if ( !ns_common::value_to_string(text, rendered) )
		return false;
	return write(rendered.data(), static_cast<long>(rendered.size()));
}

bool channel::close_write()
{
	ChannelState *s = state(impl_);
	if ( !s->channel )
		return false;
	if ( !s->channel->flush(&s->last_error) )
	{
		s->failed = true;
		return false;
	}
	s->channel->close_write();
	return true;
}

void channel::close()
{
	ChannelState *s = state(impl_);
	if ( s->channel )
	{
		s->channel->close();
		s->channel.reset();
	}
}

} // namespace madc
