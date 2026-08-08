#ifndef __LIBMADCDIS_SINK_H
#define __LIBMADCDIS_SINK_H 1

#include "libmadc/error.h"
#include "madcdis/dataset.h"

#include <functional>

namespace madc {

template <typename T>
class Sink
{
public:
	virtual ~Sink() {}

	virtual bool put(const T &input, error *err = nullptr) = 0;
	virtual bool close(error *err = nullptr) = 0;
};

template <typename T>
class FunctionSink : public Sink<T>
{
public:
	typedef std::function<bool(const T &, error *)> put_function;
	typedef std::function<bool(error *)> close_function;

	explicit FunctionSink(put_function putter,
			      close_function closer = close_function())
		: putter_(putter), closer_(closer), closed_(false)
	{}

	bool put(const T &input, error *err = nullptr) override
	{
		return !closed_ && putter_ && putter_(input, err);
	}

	bool close(error *err = nullptr) override
	{
		if ( closed_ )
			return true;
		closed_ = true;
		return !closer_ || closer_(err);
	}

private:
	put_function putter_;
	close_function closer_;
	bool closed_;
};

template <typename T>
FunctionSink<T> to_function(
		typename FunctionSink<T>::put_function putter,
		typename FunctionSink<T>::close_function closer =
			typename FunctionSink<T>::close_function())
{
	return FunctionSink<T>(putter, closer);
}

template <typename Container>
class BackInsertSink : public Sink<typename Container::value_type>
{
public:
	typedef typename Container::value_type item_type;

	explicit BackInsertSink(Container &container)
		: container_(container), closed_(false)
	{}

	bool put(const item_type &input, error *err = nullptr) override
	{
		(void)err;
		if ( closed_ )
			return false;
		container_.push_back(input);
		return true;
	}

	bool close(error *err = nullptr) override
	{
		(void)err;
		closed_ = true;
		return true;
	}

private:
	Container &container_;
	bool closed_;
};

template <typename Container>
BackInsertSink<Container> to_container(Container &container)
{
	return BackInsertSink<Container>(container);
}

template <typename T>
class DataSetSink : public Sink<T>
{
public:
	explicit DataSetSink(DataSet<T> &dataset)
		: dataset_(dataset), closed_(false)
	{}

	bool put(const T &input, error *err = nullptr) override
	{
		return !closed_ && dataset_.insert(input, err);
	}

	bool close(error *err = nullptr) override
	{
		(void)err;
		closed_ = true;
		return true;
	}

private:
	DataSet<T> &dataset_;
	bool closed_;
};

} // namespace madc

#endif // __LIBMADCDIS_SINK_H
