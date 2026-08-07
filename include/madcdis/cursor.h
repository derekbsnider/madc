#ifndef __LIBMADCDIS_CURSOR_H
#define __LIBMADCDIS_CURSOR_H 1

#include "libmadc/error.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace madc {

template <typename T>
class Cursor
{
public:
	virtual ~Cursor() {}

	virtual bool next(T &out) = 0;
	virtual void close() = 0;
};

enum class CursorStatus
{
	item,
	end,
	failure
};

template <typename T>
class ErrorAwareCursor
{
public:
	virtual ~ErrorAwareCursor() {}

	virtual CursorStatus next_status(T &out, error *err = nullptr) = 0;
};

template <typename T>
CursorStatus cursor_next(Cursor<T> &cursor, T &out, error *err = nullptr)
{
	ErrorAwareCursor<T> *aware = dynamic_cast<ErrorAwareCursor<T> *>(&cursor);
	if ( aware )
		return aware->next_status(out, err);
	return cursor.next(out) ? CursorStatus::item : CursorStatus::end;
}

namespace detail {

template <typename T>
class VectorCursor : public Cursor<T>, public ErrorAwareCursor<T>
{
public:
	explicit VectorCursor(std::vector<T> rows)
		: _rows(std::move(rows)), _index(0), _closed(false)
	{}

	bool next(T &out)
	{
		return next_status(out) == CursorStatus::item;
	}

	CursorStatus next_status(T &out, error *err = nullptr)
	{
		(void)err;
		if ( _closed || _index >= _rows.size() )
			return CursorStatus::end;
		out = _rows[_index++];
		return CursorStatus::item;
	}

	void close()
	{
		_closed = true;
		_rows.clear();
		_index = 0;
	}

private:
	std::vector<T> _rows;
	std::size_t _index;
	bool _closed;
};

} // namespace detail
} // namespace madc

#endif // __LIBMADCDIS_CURSOR_H
