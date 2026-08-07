#ifndef __MADCDIS_DATACHANNEL_H
#define __MADCDIS_DATACHANNEL_H 1

#include "libmadc/datasource.h"
#include "libmadc/error.h"

#include <cstddef>
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
	virtual void close_read() {}
	virtual void close_write() {}
	virtual void close() = 0;
};

bool write_all(DataChannel &channel, const void *buffer, std::size_t size,
	       error *err = nullptr);
bool copy_channel(DataChannel &source, DataChannel &destination,
		  error *err = nullptr);

class MemoryDataChannel : public DataChannel
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
