// Copyright (c) 2015 Cloudera, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The motivation for the using declarations below is to allow accessing the most relevant
// and most frequently used library classes without having to explicitly pull them into
// the global namespace. The goal is that when readers sees a usage of vector (etc.) without
// any further specialization they can rely on the fact that it will be a std::vector.
//
// Instead of actually including the header files for the symbols, this file only checks
// if certain include guards are defined before applying the using declaration. This makes sure
// that including this file has no impact on the compile time.
//
// Please make sure that this file is included last in the cc file's include list to make
// sure that all relevant include guards are defined.
//
// The content of this file is manually curated and should only be changed on rare
// occasions.

#ifdef _GLIBCXX_VECTOR
using std::vector;
#endif

#ifdef _GLIBCXX_MAP
using std::map;
using std::multimap;
#endif

#ifdef _GLIBCXX_LIST
using std::list;
#endif

#ifdef _GLIBCXX_SET
using std::set;
using std::multiset;
#endif

#ifdef _GLIBCXX_STACK
using std::stack;
#endif

#ifdef _GLIBCXX_STRING
using std::string;
#endif

#ifdef _GLIBCXX_IOSTREAM
using std::cout;
using std::cin;
using std::cerr;
#endif

#ifdef _GLIBCXX_OSTREAM
using std::ostream;
using std::endl;
#endif

#ifdef _GLIBCXX_IOS
using std::fixed;
using std::hex;
using std::oct;
using std::dec;
using std::left;
using std::ios;
#endif

#ifdef _GLIBCXX_IOMANIP
using std::setprecision;
using std::setfill;
using std::setw;
#endif


#ifdef _GLIBCXX_FSTREAM
using std::fstream;
using std::ifstream;
using std::ofstream;
#endif


#ifdef _GLIBCXX_SSTREAM
using std::stringstream;
using std::istringstream;
using std::ostringstream;
#endif

#ifdef BOOST_THREAD_THREAD_COMMON_HPP
using boost::thread;
#endif

#ifdef BOOST_THREAD_DETAIL_THREAD_GROUP_HPP
using boost::thread_group;
#endif

#ifdef BOOST_THREAD_MUTEX_HPP
using boost::mutex;
using boost::try_mutex;
#endif

#ifdef BOOST_LEXICAL_CAST_INCLUDED
using boost::lexical_cast;
#endif

#ifdef BOOST_THREAD_PTHREAD_SHARED_MUTEX_HPP
using boost::shared_mutex;
#endif

#ifdef BOOST_THREAD_LOCKS_HPP
using boost::lock_guard;
using boost::unique_lock;
using boost::shared_lock;
using boost::upgrade_lock;
#endif

#ifdef BOOST_SMART_PTR_SHARED_PTR_HPP_INCLUDED
using boost::shared_ptr;
#endif

#ifdef BOOST_SMART_PTR_SCOPED_PTR_HPP_INCLUDED
using boost::scoped_ptr;
#endif

#ifdef BOOST_UNORDERED_MAP_HPP_INCLUDED
using boost::unordered_map;
#endif

#ifdef BOOST_UNORDERED_SET_HPP_INCLUDED
using boost::unordered_set;
#endif

#ifdef BOOST_FUNCTION_PROLOGUE_HPP
using boost::function;
#endif

#ifdef BOOST_BIND_HPP_INCLUDED
using boost::bind;
using boost::mem_fn;
#endif
