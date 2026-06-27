#pragma once
#include "wikore/domain/types.hpp"
#include <drogon/orm/Exception.h>

namespace wikore::postgres {

// ---------------------------------------------------------------------------
// map_db_exception
//
// Converts a DrogonDbException to a typed Error value.
// Dynamic-casts to SqlError to access the SQLSTATE and constraint name.
// Every named constraint from V001-V015 must have a mapping in the .cpp;
// unmapped constraints return Error::database_error so nothing is swallowed.
// ---------------------------------------------------------------------------

Error map_db_exception(const drogon::orm::DrogonDbException& ex);

} // namespace wikore::postgres
