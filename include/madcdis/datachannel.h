#ifndef __MADCDIS_DATACHANNEL_H
#define __MADCDIS_DATACHANNEL_H 1

#include "libmadc/datasource.h"
#include "libmadc/error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace madc {

struct ChannelCapabilities
{
	bool read = false;
	bool write = false;
	bool half_close = false;
	bool seek = false;
};

enum class ChannelOpenMode
{
	read,
	write,
	read_write,
	append
};

class DataChannel
{
public:
	virtual ~DataChannel() {}

	virtual const char *name() const = 0;
	virtual ChannelCapabilities capabilities() const = 0;
	virtual bool read(void *buffer, std::size_t capacity,
			  std::size_t &bytes_read, error *err = nullptr) = 0;
	virtual bool write(const void *buffer, std::size_t size,
			   std::size_t &bytes_written, error *err = nullptr) = 0;
	virtual bool flush(error *err = nullptr)
	{
		(void)err;
		return true;
	}
	// Thread-safety contract: a DataChannel is single-threaded EXCEPT
	// close_read()/close_write(), which one other thread may call as a
	// wake-up for a blocked read/write (pump_process's failure paths do).
	// No other member may be called concurrently.
	virtual void close_read() {}
	virtual void close_write() {}
	virtual void close() = 0;
};

// Optional extension for message-oriented channels. Each call transfers one
// complete datagram; a zero-byte datagram is data, not stream EOF.
class DatagramDataChannel
{
public:
	virtual ~DatagramDataChannel() {}

	virtual bool receive_datagram(
		void *buffer, std::size_t capacity, std::size_t &bytes_read,
		error *err = nullptr) = 0;
	virtual bool send_datagram(
		const void *buffer, std::size_t size, std::size_t &bytes_written,
		error *err = nullptr) = 0;
};

// Optional extension for random-access channels. Positioned transfers
// (read_at/write_at) carry their own offset and never disturb the sequential
// position used by read()/write(); seek() repositions that sequential stream.
// Implementing this interface only says the channel CAN support random
// access — capabilities().seek is the per-instance truth (a file channel on
// a FIFO path implements the interface but reports seek=false). Probe BOTH
// before relying on it.
class SeekableDataChannel
{
public:
	virtual ~SeekableDataChannel() {}

	virtual bool size(uint64_t &out, error *err = nullptr) = 0;
	virtual bool seek(uint64_t offset, error *err = nullptr) = 0;
	virtual bool read_at(uint64_t offset, void *buffer, std::size_t capacity,
			     std::size_t &bytes_read, error *err = nullptr) = 0;
	virtual bool write_at(uint64_t offset, const void *buffer,
			      std::size_t size, std::size_t &bytes_written,
			      error *err = nullptr) = 0;
};

// The one truthful-seekability probe: interface present AND the instance
// claims it. Returns nullptr otherwise — consumers never dynamic_cast the
// mixin themselves.
SeekableDataChannel *seekable_surface(DataChannel *channel);

bool write_all(DataChannel &channel, const void *buffer, std::size_t size,
	       error *err = nullptr);
// Pump source to destination until EOF, flushing at the end. The counted
// overload reports the bytes moved and accepts a null destination
// (drain-and-count); the plain overload delegates to it.
bool copy_channel(DataChannel &source, DataChannel *destination,
		  std::size_t &byte_count, error *err = nullptr);
bool copy_channel(DataChannel &source, DataChannel &destination,
		  error *err = nullptr);

// In-memory byte channel. Sequential read() consumes from the read position;
// sequential write() always appends. The seekable surface addresses the whole
// byte image: seek() moves the read position, read_at()/write_at() are
// position-independent (write_at extends and zero-fills past the end).
class MemoryDataChannel : public DataChannel, public SeekableDataChannel
{
public:
	MemoryDataChannel();
	explicit MemoryDataChannel(const std::vector<unsigned char> &bytes);
	~MemoryDataChannel() override;

	const char *name() const override;
	ChannelCapabilities capabilities() const override;
	bool read(void *buffer, std::size_t capacity,
		  std::size_t &bytes_read, error *err = nullptr) override;
	bool write(const void *buffer, std::size_t size,
		   std::size_t &bytes_written, error *err = nullptr) override;
	bool size(uint64_t &out, error *err = nullptr) override;
	bool seek(uint64_t offset, error *err = nullptr) override;
	bool read_at(uint64_t offset, void *buffer, std::size_t capacity,
		     std::size_t &bytes_read, error *err = nullptr) override;
	bool write_at(uint64_t offset, const void *buffer, std::size_t size,
		      std::size_t &bytes_written, error *err = nullptr) override;
	void close_read() override;
	void close_write() override;
	void close() override;

	const std::vector<unsigned char> &bytes() const;
	void reset_read();
	void clear();

private:
	std::vector<unsigned char> bytes_;
	std::size_t read_position_;
	bool read_closed_;
	bool write_closed_;
};

class DataChannelRegistry
{
public:
	class Factory
	{
	public:
		virtual ~Factory() {}
		virtual std::unique_ptr<DataChannel> open(
			const DataSource &source, ChannelOpenMode mode,
			error *err = nullptr) const = 0;
	};

	static DataChannelRegistry &instance();

	void register_factory(const std::string &scheme,
			      std::unique_ptr<Factory> factory);
	bool has_factory(const std::string &scheme) const;
	std::unique_ptr<DataChannel> open(const DataSource &source,
					  ChannelOpenMode mode,
					  error *err = nullptr) const;
	std::vector<std::string> schemes() const;

private:
	DataChannelRegistry();
	struct impl;
	std::unique_ptr<impl> _;
};

} // namespace madc

#endif // __MADCDIS_DATACHANNEL_H
