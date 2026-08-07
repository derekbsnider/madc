#ifndef __LIBMADCDIS_FORMAT_FLOW_H
#define __LIBMADCDIS_FORMAT_FLOW_H 1

#include "madcdis/cursor.h"
#include "madcdis/mapper.h"
#include "madcdis/sink.h"

#include <istream>
#include <ostream>

namespace madc {

template <typename T>
class FormatCursor : public Cursor<T>, public ErrorAwareCursor<T>
{
public:
	FormatCursor(std::istream &input, const FormatAdapter<T> &adapter)
		: input_(input), adapter_(adapter), closed_(false)
	{}

	bool next(T &out) override
	{
		return next_status(out, nullptr) == CursorStatus::item;
	}

	CursorStatus next_status(T &out, error *err = nullptr) override
	{
		if ( closed_ || input_.eof() )
			return CursorStatus::end;
		error local;
		error *target = err ? err : &local;
		if ( adapter_.read_one(input_, out, target) )
			return CursorStatus::item;
		if ( input_.eof() && target->message.empty() )
			return CursorStatus::end;
		if ( target->message.empty() )
			*target = error(error::severity::error, error::phase::runtime,
					"format adapter `" + std::string(adapter_.name())
					+ "` failed to read an item");
		return CursorStatus::failure;
	}

	void close() override { closed_ = true; }

private:
	std::istream &input_;
	const FormatAdapter<T> &adapter_;
	bool closed_;
};

template <typename T>
class FormatSink : public Sink<T>
{
public:
	FormatSink(std::ostream &output, const FormatAdapter<T> &adapter)
		: output_(output), adapter_(adapter), closed_(false)
	{}

	bool put(const T &input, error *err = nullptr) override
	{
		return !closed_ && adapter_.write_one(output_, input, err);
	}

	bool close(error *err = nullptr) override
	{
		if ( closed_ )
			return true;
		output_.flush();
		closed_ = true;
		if ( output_.good() )
			return true;
		if ( err && err->message.empty() )
			*err = error(error::severity::error, error::phase::runtime,
				     "format adapter `" + std::string(adapter_.name())
				     + "` failed to flush output");
		return false;
	}

private:
	std::ostream &output_;
	const FormatAdapter<T> &adapter_;
	bool closed_;
};

} // namespace madc

#endif // __LIBMADCDIS_FORMAT_FLOW_H
