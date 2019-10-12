

/**
 * \file filter_model.cc
 * \brief Implements functions defined in filter_model.h
*/

#include "filter_model.h"
#include <iostream>
#include "utilities.h"

ukfPrecisionType SignalModel::CheckZero(const ukfPrecisionType &local_d) const
{
  if (local_d < 0)
  {
    std::cout << "A value turns zero" << std::endl;
    return ukfZero;
  }
  return local_d;
}
