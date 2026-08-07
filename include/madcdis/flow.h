#ifndef __LIBMADCDIS_FLOW_H
#define __LIBMADCDIS_FLOW_H 1

#include "madcdis/cursor.h"
#include "madcdis/sink.h"

#include <functional>
#include <memory>
#include <utility>

namespace madc {

template <typename T>
class FunctionCursor : public Cursor<T>, public ErrorAwareCursor<T>
{
public:
	typedef std::function<CursorStatus(T &, error *)> generator_function;

	explicit FunctionCursor(generator_function generator)
		: generator_(generator), closed_(false)
	{}

	~FunctionCursor() override { close(); }

	bool next(T &out) override
	{
		return next_status(out, nullptr) == CursorStatus::item;
	}

	CursorStatus next_status(T &out, error *err = nullptr) override
	{
		if ( closed_ || !generator_ )
			return CursorStatus::end;
		return generator_(out, err);
	}

	void close() override
	{
		closed_ = true;
	}

private:
	generator_function generator_;
	bool closed_;
};

template <typename T>
std::unique_ptr<Cursor<T> > from_generator(
		typename FunctionCursor<T>::generator_function generator)
{
	return std::unique_ptr<Cursor<T> >(new FunctionCursor<T>(generator));
}

template <typename T>
std::unique_ptr<Cursor<T> > from_generator(
		std::function<bool(T &, error *)> generator)
{
	typename FunctionCursor<T>::generator_function adapted =
		[generator](T &out, error *err) {
			error local;
			error *target = err ? err : &local;
			if ( generator(out, target) )
				return CursorStatus::item;
			return target->message.empty()
				? CursorStatus::end : CursorStatus::failure;
		};
	return from_generator<T>(adapted);
}

template <typename T, typename Predicate>
class FilterCursor : public Cursor<T>, public ErrorAwareCursor<T>
{
public:
	FilterCursor(std::unique_ptr<Cursor<T> > upstream, Predicate predicate)
		: upstream_(std::move(upstream)), predicate_(predicate), closed_(false)
	{}

	~FilterCursor() override { close(); }

	bool next(T &out) override
	{
		return next_status(out, nullptr) == CursorStatus::item;
	}

	CursorStatus next_status(T &out, error *err = nullptr) override
	{
		if ( closed_ || !upstream_.get() )
			return CursorStatus::end;
		for ( ;; )
		{
			CursorStatus status = cursor_next(*upstream_, out, err);
			if ( status != CursorStatus::item )
				return status;
			if ( predicate_(out) )
				return CursorStatus::item;
		}
	}

	void close() override
	{
		if ( closed_ )
			return;
		if ( upstream_.get() )
			upstream_->close();
		closed_ = true;
	}

private:
	std::unique_ptr<Cursor<T> > upstream_;
	Predicate predicate_;
	bool closed_;
};

template <typename T, typename Predicate>
std::unique_ptr<Cursor<T> > filter(std::unique_ptr<Cursor<T> > upstream,
				  Predicate predicate)
{
	return std::unique_ptr<Cursor<T> >(
		new FilterCursor<T, Predicate>(std::move(upstream), predicate));
}

template <typename In, typename Out, typename Transformer>
class TransformCursor : public Cursor<Out>, public ErrorAwareCursor<Out>
{
public:
	TransformCursor(std::unique_ptr<Cursor<In> > upstream,
			Transformer transformer)
		: upstream_(std::move(upstream)), transformer_(transformer), closed_(false)
	{}

	~TransformCursor() override { close(); }

	bool next(Out &out) override
	{
		return next_status(out, nullptr) == CursorStatus::item;
	}

	CursorStatus next_status(Out &out, error *err = nullptr) override
	{
		if ( closed_ || !upstream_.get() )
			return CursorStatus::end;
		In input;
		CursorStatus status = cursor_next(*upstream_, input, err);
		if ( status == CursorStatus::item )
			out = transformer_(input);
		return status;
	}

	void close() override
	{
		if ( closed_ )
			return;
		if ( upstream_.get() )
			upstream_->close();
		closed_ = true;
	}

private:
	std::unique_ptr<Cursor<In> > upstream_;
	Transformer transformer_;
	bool closed_;
};

template <typename In, typename Out, typename Transformer>
std::unique_ptr<Cursor<Out> > transform(std::unique_ptr<Cursor<In> > upstream,
				       Transformer transformer)
{
	return std::unique_ptr<Cursor<Out> >(
		new TransformCursor<In, Out, Transformer>(std::move(upstream), transformer));
}

template <typename T>
bool copy(std::unique_ptr<Cursor<T> > input, Sink<T> &output,
	  error *err = nullptr)
{
	if ( !input.get() )
		return false;

	error local;
	error *target = err ? err : &local;
	T item;
	for ( ;; )
	{
		CursorStatus status = cursor_next(*input, item, target);
		if ( status == CursorStatus::end )
		{
			input->close();
			return output.close(target);
		}
		if ( status == CursorStatus::failure || !output.put(item, target) )
		{
			input->close();
			error close_error;
			output.close(&close_error);
			return false;
		}
	}
}

template <typename T>
bool copy(std::unique_ptr<Cursor<T> > input, Sink<T> &&output,
	  error *err = nullptr)
{
	return copy(std::move(input), output, err);
}

} // namespace madc

#endif // __LIBMADCDIS_FLOW_H
