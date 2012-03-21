/* 
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Benjamin Segovia <benjamin.segovia@intel.com>
 */

/**
 * \file unit.hpp
 * \author Benjamin Segovia <benjamin.segovia@intel.com>
 */
#ifndef __GBE_IR_UNIT_HPP__
#define __GBE_IR_UNIT_HPP__

#include "ir/constant.hpp"
#include "sys/hash_map.hpp"

namespace gbe {
namespace ir {

  // A unit contains a set of functions
  class Function;

  /*! Defines the size of the pointers. All the functions from the unit will
   *  use the same pointer size as the unit they belong to
   */
  enum PointerSize {
    POINTER_32_BITS = 32,
    POINTER_64_BITS = 64
  };

  /*! Complete unit of compilation. It contains a set of functions and a set of
   *  constant the functions may refer to.
   */
  class Unit : public NonCopyable
  {
  public:
    typedef hash_map<std::string, Function*> FunctionSet;
    /*! Create an empty unit */
    Unit(PointerSize pointerSize = POINTER_32_BITS);
    /*! Release everything (*including* the function pointers) */
    ~Unit(void);
    /*! Get the set of functions defined in the unit */
    const FunctionSet &getFunctionSet(void) const { return functions; }
    /*! Retrieve the function by its name */
    Function *getFunction(const std::string &name) const;
    /*! Return NULL if the function already exists */
    Function *newFunction(const std::string &name);
    /*! Create a new constant in the constant set */
    void newConstant(const char*, const std::string&, uint32_t size, uint32_t alignment);
    /*! Apply the given functor on all the functions */
    template <typename T>
    INLINE void apply(const T &functor) const {
      for (auto it = functions.begin(); it != functions.end(); ++it)
        functor(*it->second);
    }
    /*! Return the size of the pointers manipulated */
    INLINE PointerSize getPointerSize(void) const { return pointerSize; }
  private:
    hash_map<std::string, Function*> functions; //!< All the defined functions
    ConstantSet constantSet; //!< All the constants defined in the unit
    PointerSize pointerSize; //!< Size shared by all pointers
    GBE_CLASS(Unit);
  };

  /*! Output the unit string in the given stream */
  std::ostream &operator<< (std::ostream &out, const Unit &unit);

} /* namespace ir */
} /* namespace gbe */

#endif /* __GBE_IR_UNIT_HPP__ */

