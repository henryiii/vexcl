#define BOOST_TEST_MODULE VectorView
#include <valarray>
#include <boost/test/unit_test.hpp>
#include "context_setup.hpp"

BOOST_AUTO_TEST_CASE(vector_view_1d)
{
    const size_t N = 1024;

    std::vector<cl::CommandQueue> queue(1, ctx.queue(0));

    std::vector<double> x = random_vector<double>(2 * N);
    vex::vector<double> X(queue, x);
    vex::vector<double> Y(queue, N);

    cl_ulong size   = N;
    cl_long  stride = 2;

    vex::gslice<1> slice(0, &size, &stride);

    Y = slice(X);

    check_sample(Y, [&](size_t idx, double v) { BOOST_CHECK(v == x[idx * 2]); });
}

BOOST_AUTO_TEST_CASE(vector_view_2)
{
    const size_t N = 32;

    std::vector<cl::CommandQueue> queue(1, ctx.queue(0));

    std::valarray<double> x(N * N);
    std::iota(&x[0], &x[N * N], 0);

    // Select every even point from sub-block [(2,4) - (10,10)]:
    size_t start    = 2 * N + 4;
    size_t size[]   = {5, 4};
    size_t stride[] = {2, 2};

    std::gslice std_slice(start, std::valarray<size_t>(size, 2), std::valarray<size_t>(stride, 2));

    std::valarray<double> y = x[std_slice];

    vex::vector<double> X(queue, N * N, &x[0]);
    vex::vector<double> Y(queue, size[0] * size[1]);

    vex::gslice<2> vex_slice(start, size, stride);

    Y = vex_slice(X);

    check_sample(Y, [&](size_t idx, double v) { BOOST_CHECK_EQUAL(v, y[idx]); });
}

BOOST_AUTO_TEST_SUITE_END()
