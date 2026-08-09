#include "madcdis/source_adapter.h"

#include <utility>

namespace madc {

std::unique_ptr<Cursor<ExtractedRecord> > extract_adapter_cursor(
	const SourceAdapter &adapter,
	const DataSource &source,
	const std::string &type_name,
	error *err)
{
	const StreamingSourceAdapter *streaming =
		dynamic_cast<const StreamingSourceAdapter *>(&adapter);
	if ( streaming )
		return streaming->extract_stream(source, type_name, err);

	std::vector<ExtractedRecord> records;
	if ( !adapter.extract(source, type_name, records, err) )
		return std::unique_ptr<Cursor<ExtractedRecord> >();
	return std::unique_ptr<Cursor<ExtractedRecord> >(
		new detail::VectorCursor<ExtractedRecord>(std::move(records)));
}

} // namespace madc
