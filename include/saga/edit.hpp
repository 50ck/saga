#pragma once
#include <saga/common.hpp>
namespace saga {
struct FileDiff {std::string text;size_t added=0,removed=0;bool coarse=false;};
FileDiff unified_diff(std::string_view before,std::string_view after,std::string_view path,const std::function<void()>& service = {});
}
