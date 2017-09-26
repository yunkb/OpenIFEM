#include <iostream>

#include "linearElasticSolver.h"
#include "hyperelasticSolver.h"

extern template class IFEM::LinearElasticSolver<2>;
extern template class IFEM::LinearElasticSolver<3>;
extern template class IFEM::HyperelasticSolver<2>;
extern template class IFEM::HyperelasticSolver<3>;

int main(int argc, char* argv[])
{
  try
  {
    IFEM::HyperelasticSolver<2> solver;
    if (argc > 1)
    {
      solver.runStatics(std::string(argv[1]));
    }
    else
    {
      solver.runStatics();
    }
  }
  catch (std::exception& exc)
  {
    std::cerr << std::endl << std::endl
              << "----------------------------------------------------"
              << std::endl;
    std::cerr << "Exception on processing: " << std::endl
              << exc.what() << std::endl
              << "Aborting!" << std::endl
              << "----------------------------------------------------"
              << std::endl;
    return 1;
  }
  catch (...)
  {
    std::cerr << std::endl << std::endl
              << "----------------------------------------------------"
              << std::endl;
    std::cerr << "Unknown exception!" << std::endl
              << "Aborting!" << std::endl
              << "----------------------------------------------------"
              << std::endl;
    return 1;
  }
  return 0;
}
