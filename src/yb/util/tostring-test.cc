//
// Copyright (c) YugaByte, Inc.
//

#include <deque>
#include <list>
#include <map>
#include <vector>
#include <unordered_map>

#include <gtest/gtest.h>

#include "yb/gutil/ref_counted.h"

#include "yb/util/test_util.h"
#include "yb/util/tostring.h"

namespace yb {
// We should use namespace other than yb::util::ToString to check how does ToString works
// with code from other namespaces.
namespace util_test {

using yb::util::ToString;

class ToStringTest : public YBTest {
};

namespace {

template<class T>
void CheckPlain(T t) {
  std::stringstream ss;
  ss << +t;
  ASSERT_EQ(ss.str(), ToString(t));
}

template<class T>
void CheckInt(T t) {
  CheckPlain<T>(t);
  CheckPlain<T>(std::numeric_limits<T>::min());
  CheckPlain<T>(std::numeric_limits<T>::max());
}

template<class T>
void CheckPointer(const std::string& tail, const T& t) {
  if (t) {
    std::stringstream ss;
    ss << "0x" << std::setw(sizeof(void *) * 2) << std::setfill('0') << std::setbase(16)
       << reinterpret_cast<size_t>(&*t) << " -> " << tail;
    ASSERT_EQ(ss.str(), ToString(t));
  } else {
    ASSERT_EQ(tail, ToString(t));
  }
}

} // namespace

TEST_F(ToStringTest, TestNumber) {
  CheckInt<int>(1984);
  CheckInt<int16>(2349);
  CheckInt<uint32_t>(23984296);
  CheckInt<size_t>(2936429238477);
  CheckInt<ptrdiff_t>(-962394729);
  CheckInt<int8_t>(45);
}

TEST_F(ToStringTest, TestCollection) {
  const std::string expected = "[1, 2, 3, 4, 5]";
  std::vector<int> v = {1, 2, 3, 4, 5};
  ASSERT_EQ(expected, ToString(v));
  CheckPointer(expected, &v);

  std::deque<int> d(v.begin(), v.end());
  ASSERT_EQ(expected, ToString(d));
  CheckPointer(expected, &d);

  std::list<int> l(v.begin(), v.end());
  ASSERT_EQ(expected, ToString(l));
  CheckPointer(expected, &l);

  auto pair = std::make_pair(v, d);
  ASSERT_EQ("{" + expected + ", " + expected + "}", ToString(pair));
}

TEST_F(ToStringTest, TestMap) {
  std::map<int, std::string> m = {{1, "one"}, {2, "two"}, {3, "three"}};
  ASSERT_EQ("[{1, one}, {2, two}, {3, three}]", ToString(m));

  std::unordered_map<int, std::string> u(m.begin(), m.end());
  auto uts = ToString(u);
  std::vector<pair<int, std::string>> v(m.begin(), m.end());
  size_t match_count = 0;
  for (;;) {
    if (uts == ToString(v)) {
      ++match_count;
    }
    if (!std::next_permutation(v.begin(), v.end())) {
      break;
    }
  }
  ASSERT_EQ(1, match_count);
}

TEST_F(ToStringTest, TestPointer) {
  const char* some_text = "some text";

  ASSERT_EQ(some_text, ToString(some_text));
  int* null_int = nullptr;
  CheckPointer("<NULL>", null_int);

  std::string expected = "23";
  int number = 23;
  CheckPointer(expected, &number);

  std::unique_ptr<int> unique_ptr(new int(number));
  CheckPointer(expected, unique_ptr);

  std::shared_ptr<int> shared_ptr = std::make_shared<int>(number);
  CheckPointer(expected, shared_ptr);
}

class ToStringable : public RefCountedThreadSafe<ToStringable> {
 public:
  std::string ToString() const {
    return std::string("ToStringable");
  }
};

class ToStringableChild : public ToStringable {
};

class WithShortDebugString {
 public:
  std::string ShortDebugString() const {
    return std::string("ShortDebugString");
  }
};

class WithShortDebugStringChild : public WithShortDebugString {
};

TEST_F(ToStringTest, TestCustomIntrusive) {
  scoped_refptr<ToStringable> ptr(new ToStringable);
  scoped_refptr<ToStringableChild> child_ptr(new ToStringableChild);
  ASSERT_EQ("ToStringable", ToString(*ptr));
  CheckPointer("ToStringable", ptr);
  CheckPointer("ToStringable", child_ptr);
  ASSERT_EQ("ShortDebugString", ToString(WithShortDebugString()));
  ASSERT_EQ("ShortDebugString", ToString(WithShortDebugStringChild()));

  std::vector<scoped_refptr<ToStringable>> v(2);
  v[1] = ptr;
  ASSERT_EQ("[<NULL>, " + ToString(v[1]) + "]", ToString(v));
}

class ToStringableNonIntrusive {
};

std::string ToString(ToStringableNonIntrusive) {
  return "ToStringableNonIntrusive";
}

TEST_F(ToStringTest, TestCustomNonIntrusive) {
  std::vector<ToStringableNonIntrusive> v(2);
  ASSERT_EQ("ToStringableNonIntrusive", ToString(v[0]));
  ASSERT_EQ("[ToStringableNonIntrusive, ToStringableNonIntrusive]", ToString(v));
}

} // namespace util_test
} // namespace yb
