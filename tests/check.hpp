#pragma once
#include <saga/common.hpp>
#include <iostream>
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(std::string("CHECK failed: ")+ #expr + " at " + __FILE__ + ":" + std::to_string(__LINE__)); } while (false)
template<class Function> void rejects(Function f) { bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; } CHECK(rejected); }
