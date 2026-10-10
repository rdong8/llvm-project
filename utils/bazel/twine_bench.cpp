#include "benchmark/benchmark.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FormatVariadic.h"

#include <charconv>
#include <concepts>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

//=============================================================================
// String-like helpers: Only accepts string-likes (std::string, string_view,
// llvm::StringRef, const char*). No formatting / non-string-likes allowed.
//=============================================================================

template <typename T> size_t string_size(const T &x) {
  if constexpr (std::is_pointer_v<std::decay_t<T>>) {
    return std::strlen(x);
  } else {
    return x.size();
  }
}

template <typename T> void string_append(std::string &out, const T &x) {
  if constexpr (std::is_pointer_v<std::decay_t<T>>) {
    out.append(x);
  } else {
    out.append(x.data(), x.size());
  }
}

template <typename T> char *write_string(char *dst, const T &x) {
  if constexpr (std::is_pointer_v<std::decay_t<T>>) {
    size_t len = std::strlen(x);
    std::memcpy(dst, x, len);
    return dst + len;
  } else {
    std::memcpy(dst, x.data(), x.size());
    return dst + x.size();
  }
}

//=============================================================================
// 1. Naive fold-based concatenation (reserve + append)
// Supports both lvalue and rvalue first arguments.
//=============================================================================

template <typename First, typename... Rest>
std::string naive_concat(First &&first, const Rest &...rest) {
  if constexpr (std::is_rvalue_reference_v<First &&> &&
                std::is_same_v<std::remove_cvref_t<First>, std::string>) {
    size_t total_size = first.size() + (string_size(rest) + ...);
    first.reserve(total_size);
    (string_append(first, rest), ...);
    return std::move(first);
  } else {
    size_t total_size = string_size(first) + (string_size(rest) + ...);
    std::string result;
    result.reserve(total_size);
    string_append(result, first);
    (string_append(result, rest), ...);
    return result;
  }
}

//=============================================================================
// 2. resize_and_overwrite (C++23) concatenation
// Supports both lvalue and rvalue first arguments.
//=============================================================================

template <typename First, typename... Rest>
std::string resize_and_overwrite_concat(First &&first, const Rest &...rest) {
  size_t total_size = string_size(first) + (string_size(rest) + ...);
  if constexpr (std::is_rvalue_reference_v<First &&> &&
                std::is_same_v<std::remove_cvref_t<First>, std::string>) {
    size_t orig_size = first.size();
    first.resize_and_overwrite(total_size, [&](char *buf, size_t) {
      char *p = buf + orig_size;
      ((p = write_string(p, rest)), ...);
      return total_size;
    });
    return std::move(first);
  } else {
    std::string result;
    result.resize_and_overwrite(total_size, [&](char *buf, size_t) {
      char *p = buf;
      p = write_string(p, first);
      ((p = write_string(p, rest)), ...);
      return static_cast<size_t>(p - buf);
    });
    return result;
  }
}

//=============================================================================
// Helper string generators
//=============================================================================

static std::string make_string(size_t len, char c = 'x') {
  return std::string(len, c);
}

//=============================================================================
// Section 1: 2-Part Concatenation: LVALUE LHS vs RVALUE LHS
//=============================================================================

// --- Lvalue LHS ---

template <size_t Size>
static void BM_2Part_Lvalue_Twine(benchmark::State &state) {
  std::string s1 = make_string(Size, 'a');
  std::string s2_str = make_string(Size, 'b');
  std::string_view s2 = s2_str;
  for (auto _ : state) {
    auto res = (llvm::Twine(s1) + s2).str();
    benchmark::DoNotOptimize(res);
  }
  state.SetBytesProcessed(state.iterations() * Size * 2);
}

template <size_t Size>
static void BM_2Part_Lvalue_NaiveConcat(benchmark::State &state) {
  std::string s1 = make_string(Size, 'a');
  std::string s2_str = make_string(Size, 'b');
  std::string_view s2 = s2_str;
  for (auto _ : state) {
    auto res = naive_concat(s1, s2);
    benchmark::DoNotOptimize(res);
  }
  state.SetBytesProcessed(state.iterations() * Size * 2);
}

template <size_t Size>
static void BM_2Part_Lvalue_ResizeOverwrite(benchmark::State &state) {
  std::string s1 = make_string(Size, 'a');
  std::string s2_str = make_string(Size, 'b');
  std::string_view s2 = s2_str;
  for (auto _ : state) {
    auto res = resize_and_overwrite_concat(s1, s2);
    benchmark::DoNotOptimize(res);
  }
  state.SetBytesProcessed(state.iterations() * Size * 2);
}

template <size_t Size>
static void BM_2Part_Lvalue_P2591_OpPlus(benchmark::State &state) {
  std::string s1 = make_string(Size, 'a');
  std::string s2_str = make_string(Size, 'b');
  std::string_view s2 = s2_str;
  for (auto _ : state) {
    auto res = s1 + s2;
    benchmark::DoNotOptimize(res);
  }
  state.SetBytesProcessed(state.iterations() * Size * 2);
}

// --- Rvalue LHS (Exact Capacity, must reallocate to append) ---

template <size_t Size>
static void BM_2Part_Rvalue_NaiveConcat(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(Size, 'b');
  std::string_view s2 = s2_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i)
      pool[i] = make_string(Size, 'a');
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = naive_concat(std::move(pool[i]), s2);
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
  state.SetBytesProcessed(state.iterations() * BatchSize * Size * 2);
}

template <size_t Size>
static void BM_2Part_Rvalue_ResizeOverwrite(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(Size, 'b');
  std::string_view s2 = s2_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i)
      pool[i] = make_string(Size, 'a');
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = resize_and_overwrite_concat(std::move(pool[i]), s2);
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
  state.SetBytesProcessed(state.iterations() * BatchSize * Size * 2);
}

template <size_t Size>
static void BM_2Part_Rvalue_P2591_OpPlus(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(Size, 'b');
  std::string_view s2 = s2_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i)
      pool[i] = make_string(Size, 'a');
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = std::move(pool[i]) + s2;
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
  state.SetBytesProcessed(state.iterations() * BatchSize * Size * 2);
}

// --- Rvalue LHS with Reserved Capacity (Zero Allocation append) ---

template <size_t Size>
static void BM_2Part_RvalueReserved_P2591_OpPlus(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(Size, 'b');
  std::string_view s2 = s2_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i) {
      pool[i] = make_string(Size, 'a');
      pool[i].reserve(Size * 2);
    }
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = std::move(pool[i]) + s2;
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
  state.SetBytesProcessed(state.iterations() * BatchSize * Size * 2);
}

// Register 2-Part Benchmarks for 6B (SSO), 32B (Stack Buffer), 256B (Exceed
// Stack), 2048B (Heap)
#define REGISTER_2PART(SZ, SUFFIX)                                             \
  BENCHMARK_TEMPLATE(BM_2Part_Lvalue_Twine, SZ)                                \
      ->Name("2P_Lval_Twine/" SUFFIX);                                         \
  BENCHMARK_TEMPLATE(BM_2Part_Lvalue_NaiveConcat, SZ)                          \
      ->Name("2P_Lval_Naive/" SUFFIX);                                         \
  BENCHMARK_TEMPLATE(BM_2Part_Lvalue_ResizeOverwrite, SZ)                      \
      ->Name("2P_Lval_Resize/" SUFFIX);                                        \
  BENCHMARK_TEMPLATE(BM_2Part_Lvalue_P2591_OpPlus, SZ)                         \
      ->Name("2P_Lval_P2591/" SUFFIX);                                         \
  BENCHMARK_TEMPLATE(BM_2Part_Rvalue_NaiveConcat, SZ)                          \
      ->Name("2P_Rval_Naive/" SUFFIX);                                         \
  BENCHMARK_TEMPLATE(BM_2Part_Rvalue_ResizeOverwrite, SZ)                      \
      ->Name("2P_Rval_Resize/" SUFFIX);                                        \
  BENCHMARK_TEMPLATE(BM_2Part_Rvalue_P2591_OpPlus, SZ)                         \
      ->Name("2P_Rval_P2591/" SUFFIX);                                         \
  BENCHMARK_TEMPLATE(BM_2Part_RvalueReserved_P2591_OpPlus, SZ)                 \
      ->Name("2P_RvalCap_P2591/" SUFFIX);

REGISTER_2PART(6, "6B_SSO");
REGISTER_2PART(32, "32B_Stack");
REGISTER_2PART(256, "256B_ExceedStack");
REGISTER_2PART(2048, "2048B_Heap");

//=============================================================================
// Section 2: Multi-Part Concatenation (3 and 5 parts)
// Comparing Lvalue LHS vs Rvalue LHS
//=============================================================================

// 3-Part: Lvalue
static void BM_3Part_Lvalue_Twine_30B(benchmark::State &state) {
  std::string s1 = make_string(30, 'a');
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string_view s2 = s2_str, s3 = s3_str;
  for (auto _ : state) {
    auto res = (llvm::Twine(s1) + s2 + s3).str();
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_3Part_Lvalue_Twine_30B);

static void BM_3Part_Lvalue_NaiveConcat_30B(benchmark::State &state) {
  std::string s1 = make_string(30, 'a');
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string_view s2 = s2_str, s3 = s3_str;
  for (auto _ : state) {
    auto res = naive_concat(s1, s2, s3);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_3Part_Lvalue_NaiveConcat_30B);

static void BM_3Part_Lvalue_ResizeOverwrite_30B(benchmark::State &state) {
  std::string s1 = make_string(30, 'a');
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string_view s2 = s2_str, s3 = s3_str;
  for (auto _ : state) {
    auto res = resize_and_overwrite_concat(s1, s2, s3);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_3Part_Lvalue_ResizeOverwrite_30B);

static void BM_3Part_Lvalue_P2591_OpPlus_30B(benchmark::State &state) {
  std::string s1 = make_string(30, 'a');
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string_view s2 = s2_str, s3 = s3_str;
  for (auto _ : state) {
    auto res = s1 + s2 + s3;
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_3Part_Lvalue_P2591_OpPlus_30B);

// 3-Part: Rvalue
static void BM_3Part_Rvalue_NaiveConcat_30B(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string_view s2 = s2_str, s3 = s3_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i)
      pool[i] = make_string(30, 'a');
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = naive_concat(std::move(pool[i]), s2, s3);
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
}
BENCHMARK(BM_3Part_Rvalue_NaiveConcat_30B);

static void BM_3Part_Rvalue_ResizeOverwrite_30B(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string_view s2 = s2_str, s3 = s3_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i)
      pool[i] = make_string(30, 'a');
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = resize_and_overwrite_concat(std::move(pool[i]), s2, s3);
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
}
BENCHMARK(BM_3Part_Rvalue_ResizeOverwrite_30B);

static void BM_3Part_Rvalue_P2591_OpPlus_30B(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string_view s2 = s2_str, s3 = s3_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i)
      pool[i] = make_string(30, 'a');
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = std::move(pool[i]) + s2 + s3;
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
}
BENCHMARK(BM_3Part_Rvalue_P2591_OpPlus_30B);

static void BM_3Part_RvalueReserved_P2591_OpPlus_30B(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string_view s2 = s2_str, s3 = s3_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i) {
      pool[i] = make_string(30, 'a');
      pool[i].reserve(90);
    }
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = std::move(pool[i]) + s2 + s3;
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
}
BENCHMARK(BM_3Part_RvalueReserved_P2591_OpPlus_30B);

// 5-Part: Lvalue
static void BM_5Part_Lvalue_Twine_30B(benchmark::State &state) {
  std::string s1 = make_string(30, 'a');
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string s4_str = make_string(30, 'd'), s5_str = make_string(30, 'e');
  std::string_view s2 = s2_str, s3 = s3_str, s4 = s4_str, s5 = s5_str;
  for (auto _ : state) {
    auto res = (llvm::Twine(s1) + s2 + s3 + s4 + s5).str();
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_5Part_Lvalue_Twine_30B);

static void BM_5Part_Lvalue_NaiveConcat_30B(benchmark::State &state) {
  std::string s1 = make_string(30, 'a');
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string s4_str = make_string(30, 'd'), s5_str = make_string(30, 'e');
  std::string_view s2 = s2_str, s3 = s3_str, s4 = s4_str, s5 = s5_str;
  for (auto _ : state) {
    auto res = naive_concat(s1, s2, s3, s4, s5);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_5Part_Lvalue_NaiveConcat_30B);

static void BM_5Part_Lvalue_ResizeOverwrite_30B(benchmark::State &state) {
  std::string s1 = make_string(30, 'a');
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string s4_str = make_string(30, 'd'), s5_str = make_string(30, 'e');
  std::string_view s2 = s2_str, s3 = s3_str, s4 = s4_str, s5 = s5_str;
  for (auto _ : state) {
    auto res = resize_and_overwrite_concat(s1, s2, s3, s4, s5);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_5Part_Lvalue_ResizeOverwrite_30B);

static void BM_5Part_Lvalue_P2591_OpPlus_30B(benchmark::State &state) {
  std::string s1 = make_string(30, 'a');
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string s4_str = make_string(30, 'd'), s5_str = make_string(30, 'e');
  std::string_view s2 = s2_str, s3 = s3_str, s4 = s4_str, s5 = s5_str;
  for (auto _ : state) {
    auto res = s1 + s2 + s3 + s4 + s5;
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_5Part_Lvalue_P2591_OpPlus_30B);

// 5-Part: Rvalue
static void BM_5Part_Rvalue_NaiveConcat_30B(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string s4_str = make_string(30, 'd'), s5_str = make_string(30, 'e');
  std::string_view s2 = s2_str, s3 = s3_str, s4 = s4_str, s5 = s5_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i)
      pool[i] = make_string(30, 'a');
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = naive_concat(std::move(pool[i]), s2, s3, s4, s5);
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
}
BENCHMARK(BM_5Part_Rvalue_NaiveConcat_30B);

static void BM_5Part_Rvalue_ResizeOverwrite_30B(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string s4_str = make_string(30, 'd'), s5_str = make_string(30, 'e');
  std::string_view s2 = s2_str, s3 = s3_str, s4 = s4_str, s5 = s5_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i)
      pool[i] = make_string(30, 'a');
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res =
          resize_and_overwrite_concat(std::move(pool[i]), s2, s3, s4, s5);
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
}
BENCHMARK(BM_5Part_Rvalue_ResizeOverwrite_30B);

static void BM_5Part_Rvalue_P2591_OpPlus_30B(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string s4_str = make_string(30, 'd'), s5_str = make_string(30, 'e');
  std::string_view s2 = s2_str, s3 = s3_str, s4 = s4_str, s5 = s5_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i)
      pool[i] = make_string(30, 'a');
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = std::move(pool[i]) + s2 + s3 + s4 + s5;
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
}
BENCHMARK(BM_5Part_Rvalue_P2591_OpPlus_30B);

static void BM_5Part_RvalueReserved_P2591_OpPlus_30B(benchmark::State &state) {
  constexpr size_t BatchSize = 64;
  std::vector<std::string> pool(BatchSize);
  std::string s2_str = make_string(30, 'b'), s3_str = make_string(30, 'c');
  std::string s4_str = make_string(30, 'd'), s5_str = make_string(30, 'e');
  std::string_view s2 = s2_str, s3 = s3_str, s4 = s4_str, s5 = s5_str;

  for (auto _ : state) {
    state.PauseTiming();
    for (size_t i = 0; i < BatchSize; ++i) {
      pool[i] = make_string(30, 'a');
      pool[i].reserve(150);
    }
    state.ResumeTiming();

    for (size_t i = 0; i < BatchSize; ++i) {
      auto res = std::move(pool[i]) + s2 + s3 + s4 + s5;
      benchmark::DoNotOptimize(res);
    }
  }
  state.SetItemsProcessed(state.iterations() * BatchSize);
}
BENCHMARK(BM_5Part_RvalueReserved_P2591_OpPlus_30B);

//=============================================================================
// Section 3: NodeKind Comparisons
// (All non-string likes converted to strings prior to concat call)
//=============================================================================

// CStringKind (const char* + const char*)
static const char *kStr1 = "Function 'foo' has invalid attribute '";
static const char *kStr2 = "' in declaration";

static void BM_NodeKind_CString_Twine(benchmark::State &state) {
  for (auto _ : state) {
    auto res = (llvm::Twine(kStr1) + kStr2).str();
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_CString_Twine);

static void BM_NodeKind_CString_NaiveConcat(benchmark::State &state) {
  for (auto _ : state) {
    auto res = naive_concat(kStr1, kStr2);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_CString_NaiveConcat);

static void BM_NodeKind_CString_ResizeOverwrite(benchmark::State &state) {
  for (auto _ : state) {
    auto res = resize_and_overwrite_concat(kStr1, kStr2);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_CString_ResizeOverwrite);

static void BM_NodeKind_CString_StdStringPlus(benchmark::State &state) {
  for (auto _ : state) {
    auto res = std::string(kStr1) + kStr2;
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_CString_StdStringPlus);

// Mixed Diagnostic: StringRef + int + char + StringRef
// Mixed Diagnostic: StringRef + int + char + StringRef
// Note: Includes the time to convert the integer operand to a string!

static void BM_NodeKind_MixedDiagnostic_Twine(benchmark::State &state) {
  llvm::StringRef prefix = "error at line ";
  int line = 142;
  char sep = ':';
  llvm::StringRef msg = " undefined symbol 'main'";
  for (auto _ : state) {
    auto res =
        (llvm::Twine(prefix) + llvm::Twine(line) + llvm::Twine(sep) + msg)
            .str();
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_MixedDiagnostic_Twine);

// Fast stack conversion via std::to_chars inside the loop
static void
BM_NodeKind_MixedDiagnostic_ToChars_NaiveConcat(benchmark::State &state) {
  std::string_view prefix = "error at line ";
  int line = 142;
  std::string_view sep = ":";
  std::string_view msg = " undefined symbol 'main'";

  for (auto _ : state) {
    char line_buf[24];
    auto [p, ec] = std::to_chars(line_buf, line_buf + sizeof(line_buf), line);
    std::string_view line_str(line_buf, p - line_buf);
    auto res = naive_concat(prefix, line_str, sep, msg);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_MixedDiagnostic_ToChars_NaiveConcat);

static void
BM_NodeKind_MixedDiagnostic_ToChars_ResizeOverwrite(benchmark::State &state) {
  std::string_view prefix = "error at line ";
  int line = 142;
  std::string_view sep = ":";
  std::string_view msg = " undefined symbol 'main'";

  for (auto _ : state) {
    char line_buf[24];
    auto [p, ec] = std::to_chars(line_buf, line_buf + sizeof(line_buf), line);
    std::string_view line_str(line_buf, p - line_buf);
    auto res = resize_and_overwrite_concat(prefix, line_str, sep, msg);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_MixedDiagnostic_ToChars_ResizeOverwrite);

static void
BM_NodeKind_MixedDiagnostic_ToChars_P2591_OpPlus(benchmark::State &state) {
  std::string prefix = "error at line ";
  int line = 142;
  std::string_view sep = ":";
  std::string_view msg = " undefined symbol 'main'";

  for (auto _ : state) {
    char line_buf[24];
    auto [p, ec] = std::to_chars(line_buf, line_buf + sizeof(line_buf), line);
    std::string_view line_str(line_buf, p - line_buf);
    auto res = prefix + line_str + sep + msg;
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_MixedDiagnostic_ToChars_P2591_OpPlus);

// Standard conversion via std::to_string inside the loop
static void
BM_NodeKind_MixedDiagnostic_ToString_NaiveConcat(benchmark::State &state) {
  std::string_view prefix = "error at line ";
  int line = 142;
  std::string_view sep = ":";
  std::string_view msg = " undefined symbol 'main'";

  for (auto _ : state) {
    std::string line_str = std::to_string(line);
    auto res = naive_concat(prefix, line_str, sep, msg);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_MixedDiagnostic_ToString_NaiveConcat);

static void
BM_NodeKind_MixedDiagnostic_ToString_ResizeOverwrite(benchmark::State &state) {
  std::string_view prefix = "error at line ";
  int line = 142;
  std::string_view sep = ":";
  std::string_view msg = " undefined symbol 'main'";

  for (auto _ : state) {
    std::string line_str = std::to_string(line);
    auto res = resize_and_overwrite_concat(prefix, line_str, sep, msg);
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_MixedDiagnostic_ToString_ResizeOverwrite);

static void
BM_NodeKind_MixedDiagnostic_ToString_P2591_OpPlus(benchmark::State &state) {
  std::string prefix = "error at line ";
  int line = 142;
  std::string_view sep = ":";
  std::string_view msg = " undefined symbol 'main'";

  for (auto _ : state) {
    auto res = prefix + std::to_string(line) + sep + msg;
    benchmark::DoNotOptimize(res);
  }
}
BENCHMARK(BM_NodeKind_MixedDiagnostic_ToString_P2591_OpPlus);

BENCHMARK_MAIN();
