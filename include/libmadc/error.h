#ifndef __LIBMADC_ERROR_H
#define __LIBMADC_ERROR_H 1

#include <string>
#include <vector>

class Program;

namespace madc {

class error
{
public:
    enum class severity
    {
	warning,
	error
    };

    enum class phase
    {
	unknown,
	lexer,
	parser,
	compiler,
	runtime
    };

    error();
    error(severity sev, phase ph, const std::string &msg,
	  const std::string &src_file = std::string(),
	  int src_line = 0,
	  int src_column = 0);

    severity level;
    phase stage;
    std::string message;
    std::string file;
    int line;
    int column;

    bool operator==(const error &other) const;
    bool operator!=(const error &other) const { return !(*this == other); }

    static const char *severity_name(severity sev);
    static const char *phase_name(phase ph);
    std::string to_string() const;
};

std::vector<error> make_errors_from_program_diagnostics(const Program &pgm);

} // namespace madc

#endif // __LIBMADC_ERROR_H
