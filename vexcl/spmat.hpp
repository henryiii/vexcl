#ifndef VEXCL_SPMAT_HPP
#define VEXCL_SPMAT_HPP

/*
The MIT License

Copyright (c) 2012-2013 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   vexcl/spmat.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  OpenCL sparse matrix.
 */

#ifdef WIN32
#  pragma warning(push)
#  pragma warning(disable : 4267 4290 4800)
#  define NOMINMAX
#endif

#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <string>
#include <memory>
#include <algorithm>
#include <iostream>
#include <type_traits>
#include <vexcl/vector.hpp>

namespace vex {
/// \cond INTERNAL
    template <typename real>
    struct sparse_matrix {
        virtual void mul_local(
                const cl::Buffer &x, const cl::Buffer &y,
                real alpha, bool append
                ) const = 0;

        virtual void mul_remote(
                const cl::Buffer &x, const cl::Buffer &y,
                real alpha, const std::vector<cl::Event> &event
                ) const = 0;

        virtual ~sparse_matrix() {}
    };
/// \endcond
}

#include <vexcl/spmat/hybrid_ell.hpp>

namespace vex {

/// \cond INTERNAL

struct matrix_terminal {};

template <class M, class V>
struct spmv
    : vector_expression< boost::proto::terminal< additive_vector_transform >::type >
{
    typedef typename M::value_type value_type;

    const M &A;
    const V &x;

    value_type scale;

    spmv(const M &m, const V &v) : A(m), x(v), scale(1) {}

    template<bool negate, bool append>
    void apply(V &y) const {
        A.mul(x, y, negate ? -scale : scale, append);
    }
};

template <class M, class V>
typename std::enable_if<
    std::is_base_of<matrix_terminal, M>::value &&
    std::is_base_of<vector_terminal_expression, V>::value &&
    std::is_same<typename M::value_type, typename V::value_type>::value,
    spmv< M, V >
>::type
operator*(const M &A, const V &x) {
    return spmv< M, V >(A, x);
}

template <class M, class V>
struct is_scalable< spmv<M, V> > : std::true_type {};

#ifdef VEXCL_MULTIVECTOR_HPP

template <class M, class V>
struct multispmv
    : multivector_expression<
        boost::proto::terminal< additive_multivector_transform >::type
        >
{
    typedef typename M::value_type value_type;

    const M &A;
    const V &x;

    value_type scale;

    multispmv(const M &m, const V &v) : A(m), x(v), scale(1) {}

    template <bool negate, bool append, class W>
    typename std::enable_if<
        std::is_base_of<multivector_terminal_expression, W>::value
#ifndef WIN32
        && std::is_same<value_type, typename W::value_type::value_type>::value
#endif
        && number_of_components<V>::value == number_of_components<W>::value,
        void
    >::type
    apply(W &y) const {
        for(size_t i = 0; i < number_of_components<V>::value; i++)
            A.mul(x(i), y(i), negate ? -scale : scale, append);
    }
};

template <class M, class V>
typename std::enable_if<
    std::is_base_of<matrix_terminal,      M>::value &&
    std::is_base_of<multivector_terminal_expression, V>::value &&
    std::is_same<typename M::value_type, typename V::value_type::value_type>::value,
    multispmv< M, V >
>::type
operator*(const M &A, const V &x) {
    return multispmv< M, V >(A, x);
}

template <class M, class V>
struct is_scalable< multispmv<M, V> > : std::true_type {};

#endif

/// \endcond

/// Sparse matrix in hybrid ELL-CSR format.
template <typename real, typename column_t = size_t, typename idx_t = size_t>
class SpMat : matrix_terminal {
    public:
        typedef real value_type;

        /// Empty constructor.
        SpMat() : nrows(0), ncols(0), nnz(0) {}

        /// Constructor.
        /**
         * Constructs GPU representation of the matrix. Input matrix is in CSR
         * format. GPU matrix utilizes ELL format and is split equally across
         * all compute devices. When there are more than one device, secondary
         * queue can be used to perform transfer of ghost values across GPU
         * boundaries in parallel with computation kernel.
         * \param queue vector of queues. Each queue represents one
         *            compute device.
         * \param n   number of rows in the matrix.
         * \param m   number of cols in the matrix.
         * \param row row index into col and val vectors.
         * \param col column numbers of nonzero elements of the matrix.
         * \param val values of nonzero elements of the matrix.
         */
        SpMat(const std::vector<cl::CommandQueue> &queue,
              size_t n, size_t m, const idx_t *row, const column_t *col, const real *val
              );

        /// Matrix-vector multiplication.
        /**
         * Matrix vector multiplication (\f$y = \alpha Ax\f$ or \f$y += \alpha
         * Ax\f$) is performed in parallel on all registered compute devices.
         * Ghost values of x are transfered across GPU boundaries as needed.
         * \param x      input vector.
         * \param y      output vector.
         * \param alpha  coefficient in front of matrix-vector product
         * \param append if set, matrix-vector product is appended to y.
         *               Otherwise, y is replaced with matrix-vector product.
         */
        void mul(const vex::vector<real> &x, vex::vector<real> &y,
                 real alpha = 1, bool append = false) const;

        /// Number of rows.
        size_t rows() const { return nrows; }
        /// Number of columns.
        size_t cols() const { return ncols; }
        /// Number of non-zero entries.
        size_t nonzeros() const { return nnz;   }
    private:
        struct SpMatCSR : public sparse_matrix<real> {
            SpMatCSR(
                    const cl::CommandQueue &queue,
                    size_t beg, size_t end, column_t xbeg, column_t xend,
                    const idx_t *row, const column_t *col, const real *val,
                    const std::set<column_t> &remote_cols
                    );

            void mul_local(
                    const cl::Buffer &x, const cl::Buffer &y,
                    real alpha, bool append
                    ) const;

            void mul_remote(
                    const cl::Buffer &x, const cl::Buffer &y,
                    real alpha, const std::vector<cl::Event> &event
                    ) const;

            const cl::CommandQueue &queue;

            size_t n;

            bool has_loc;
            bool has_rem;

            struct {
                cl::Buffer row;
                cl::Buffer col;
                cl::Buffer val;
            } loc, rem;

            static const kernel_cache_entry& spmv_set(const cl::CommandQueue& queue);
            static const kernel_cache_entry& spmv_add(const cl::CommandQueue& queue);
        };

        struct exdata {
            std::vector<column_t> cols_to_recv;
            mutable std::vector<real> vals_to_recv;

            cl::Buffer cols_to_send;
            cl::Buffer vals_to_send;
            mutable cl::Buffer rx;
        };

        const std::vector<cl::CommandQueue> queue;
        std::vector<cl::CommandQueue>       squeue;
        const std::vector<size_t>           part;

        mutable std::vector<std::vector<cl::Event>> event1;
        mutable std::vector<std::vector<cl::Event>> event2;

        std::vector<std::unique_ptr<sparse_matrix<real>>> mtx;

        std::vector<exdata> exc;
        std::vector<size_t> cidx;
        mutable std::vector<real> rx;

        size_t nrows;
        size_t ncols;
        size_t nnz;

        std::vector<std::set<column_t>> setup_exchange(
                size_t n, const std::vector<size_t> &xpart,
                const idx_t *row, const column_t *col, const real *val
                );
};

template <typename real, typename column_t, typename idx_t>
SpMat<real,column_t,idx_t>::SpMat(
        const std::vector<cl::CommandQueue> &queue,
        size_t n, size_t m, const idx_t *row, const column_t *col, const real *val
        )
    : queue(queue), part(partition(n, queue)),
      event1(queue.size(), std::vector<cl::Event>(1)),
      event2(queue.size(), std::vector<cl::Event>(1)),
      mtx(queue.size()), exc(queue.size()),
      nrows(n), ncols(m), nnz(row[n])
{
    auto xpart = partition(m, queue);

    // Create secondary queues.
    for(auto q = queue.begin(); q != queue.end(); q++)
        squeue.push_back(cl::CommandQueue(qctx(*q), qdev(*q)));

    std::vector<std::set<column_t>> remote_cols = setup_exchange(n, xpart, row, col, val);

    // Each device get it's own strip of the matrix.
#pragma omp parallel for schedule(static,1)
    for(int d = 0; d < static_cast<int>(queue.size()); d++) {
        if (part[d + 1] > part[d]) {
            cl::Device device = qdev(queue[d]);

            if ( is_cpu(device) )
                mtx[d].reset(
                        new SpMatCSR(queue[d],
                            part[d], part[d + 1],
                            xpart[d], xpart[d + 1],
                            row, col, val, remote_cols[d])
                        );
            else
                mtx[d].reset(
                        new SpMatHELL<real, column_t, idx_t>(
                            queue[d], row, col, val,
                            part[d], part[d + 1],
                            xpart[d], xpart[d + 1],
                            remote_cols[d])
                        );
        }
    }
}

template <typename real, typename column_t, typename idx_t>
void SpMat<real,column_t,idx_t>::mul(const vex::vector<real> &x, vex::vector<real> &y,
        real alpha, bool append) const
{
    static kernel_cache cache;

    if (rx.size()) {
        // Transfer remote parts of the input vector.
        for(uint d = 0; d < queue.size(); d++) {
            cl::Context context = qctx(queue[d]);
            cl::Device  device  = qdev(queue[d]);

            auto gather = cache.find(context());

            if (gather == cache.end()) {
                std::ostringstream source;

                source << standard_kernel_header(device) <<
                    "typedef " << type_name<real>() << " real;\n"
                    "kernel void gather_vals_to_send(\n"
                    "    " << type_name<size_t>() << " n,\n"
                    "    global const real *vals,\n"
                    "    global const " << type_name<column_t>() << " *cols_to_send,\n"
                    "    global real *vals_to_send\n"
                    "    )\n"
                    "{\n"
                    "    size_t i = get_global_id(0);\n"
                    "    if (i < n) vals_to_send[i] = vals[cols_to_send[i]];\n"
                    "}\n";

                auto program = build_sources(context, source.str());

                cl::Kernel krn(program, "gather_vals_to_send");
                size_t wgs = kernel_workgroup_size(krn, device);

                gather = cache.insert(std::make_pair(
                            context(), kernel_cache_entry(krn, wgs)
                            )).first;
            }

            if (size_t ncols = cidx[d + 1] - cidx[d]) {
                size_t g_size = alignup(ncols, gather->second.wgsize);

                uint pos = 0;
                gather->second.kernel.setArg(pos++, ncols);
                gather->second.kernel.setArg(pos++, x(d));
                gather->second.kernel.setArg(pos++, exc[d].cols_to_send);
                gather->second.kernel.setArg(pos++, exc[d].vals_to_send);

                queue[d].enqueueNDRangeKernel(gather->second.kernel,
                        cl::NullRange, g_size, gather->second.wgsize, 0, &event1[d][0]);

                squeue[d].enqueueReadBuffer(exc[d].vals_to_send, CL_FALSE,
                        0, ncols * sizeof(real), &rx[cidx[d]], &event1[d], &event2[d][0]
                        );
            }
        }
    }

    // Compute contribution from local part of the matrix.
    for(uint d = 0; d < queue.size(); d++)
        if (mtx[d]) mtx[d]->mul_local(x(d), y(d), alpha, append);

    // Compute contribution from remote part of the matrix.
    if (rx.size()) {
        for(uint d = 0; d < queue.size(); d++)
            if (cidx[d + 1] > cidx[d]) event2[d][0].wait();

        for(uint d = 0; d < queue.size(); d++) {
            cl::Context context = qctx(queue[d]);

            if (exc[d].cols_to_recv.size()) {
                for(size_t i = 0; i < exc[d].cols_to_recv.size(); i++)
                    exc[d].vals_to_recv[i] = rx[exc[d].cols_to_recv[i]];

                squeue[d].enqueueWriteBuffer(
                        exc[d].rx, CL_FALSE, 0, bytes(exc[d].vals_to_recv),
                        exc[d].vals_to_recv.data(), 0, &event2[d][0]
                        );

                mtx[d]->mul_remote(exc[d].rx, y(d), alpha, event2[d]);
            }
        }
    }
}

template <typename real, typename column_t, typename idx_t>
std::vector<std::set<column_t>> SpMat<real,column_t,idx_t>::setup_exchange(
        size_t, const std::vector<size_t> &xpart,
        const idx_t *row, const column_t *col, const real *
        )
{
    std::vector<std::set<column_t>> remote_cols(queue.size());

    if (queue.size() <= 1) return remote_cols;

    // Build sets of ghost points.
#pragma omp parallel for schedule(static,1)
    for(int d = 0; d < static_cast<int>(queue.size()); d++) {
        for(size_t i = part[d]; i < part[d + 1]; i++) {
            for(idx_t j = row[i]; j < row[i + 1]; j++) {
                if (col[j] < static_cast<column_t>(xpart[d]) || col[j] >= static_cast<column_t>(xpart[d + 1])) {
                    remote_cols[d].insert(col[j]);
                }
            }
        }
    }

    // Complete set of points to be exchanged between devices.
    std::vector<column_t> cols_to_send;
    {
        std::set<column_t> cols_to_send_s;
        for(uint d = 0; d < queue.size(); d++)
            cols_to_send_s.insert(remote_cols[d].begin(), remote_cols[d].end());

        cols_to_send.insert(cols_to_send.begin(), cols_to_send_s.begin(), cols_to_send_s.end());
    }

    // Build local structures to facilitate exchange.
    if (cols_to_send.size()) {
#pragma omp parallel for schedule(static,1)
        for(int d = 0; d < static_cast<int>(queue.size()); d++) {
            if (size_t rcols = remote_cols[d].size()) {
                exc[d].cols_to_recv.resize(rcols);
                exc[d].vals_to_recv.resize(rcols);

                exc[d].rx = cl::Buffer(qctx(queue[d]), CL_MEM_READ_ONLY, rcols * sizeof(real));

                for(size_t i = 0, j = 0; i < cols_to_send.size(); i++)
                    if (remote_cols[d].count(cols_to_send[i])) exc[d].cols_to_recv[j++] = i;
            }
        }

        rx.resize(cols_to_send.size());
        cidx.resize(queue.size() + 1);

        {
            auto beg = cols_to_send.begin();
            auto end = cols_to_send.end();
            for(uint d = 0; d <= queue.size(); d++) {
                cidx[d] = std::lower_bound(beg, end, xpart[d]) - cols_to_send.begin();
                beg = cols_to_send.begin() + cidx[d];
            }
        }

        for(uint d = 0; d < queue.size(); d++) {
            if (size_t ncols = cidx[d + 1] - cidx[d]) {
                cl::Context context = qctx(queue[d]);

                exc[d].cols_to_send = cl::Buffer(
                        context, CL_MEM_READ_ONLY, ncols * sizeof(column_t));

                exc[d].vals_to_send = cl::Buffer(
                        context, CL_MEM_READ_WRITE, ncols * sizeof(real));

                for(size_t i = cidx[d]; i < cidx[d + 1]; i++)
                    cols_to_send[i] -= xpart[d];

                queue[d].enqueueWriteBuffer(
                        exc[d].cols_to_send, CL_TRUE, 0, ncols * sizeof(column_t),
                        &cols_to_send[cidx[d]]);
            }
        }
    }

    return remote_cols;
}

//---------------------------------------------------------------------------
// SpMat::SpMatCSR
//---------------------------------------------------------------------------
template <typename real, typename column_t, typename idx_t>
SpMat<real,column_t,idx_t>::SpMatCSR::SpMatCSR(
        const cl::CommandQueue &queue,
        size_t beg, size_t end, column_t xbeg, column_t xend,
        const idx_t *row, const column_t *col, const real *val,
        const std::set<column_t> &remote_cols
        )
    : queue(queue), n(end - beg), has_loc(false), has_rem(false)
{
    cl::Context context = qctx(queue);

    if (beg == 0 && remote_cols.empty()) {
        if (row[n]) {
            loc.row = cl::Buffer(
                    context, CL_MEM_READ_ONLY, (n + 1) * sizeof(idx_t));

            loc.col = cl::Buffer(
                    context, CL_MEM_READ_ONLY, row[n] * sizeof(column_t));

            loc.val = cl::Buffer(
                    context, CL_MEM_READ_ONLY, row[n] * sizeof(real));

            queue.enqueueWriteBuffer(
                    loc.row, CL_FALSE, 0, (n + 1) * sizeof(idx_t), row);

            queue.enqueueWriteBuffer(
                    loc.col, CL_FALSE, 0, row[n] * sizeof(column_t), col);

            queue.enqueueWriteBuffer(
                    loc.val, CL_TRUE, 0, row[n] * sizeof(real), val);
        }

        has_loc = row[n];
        has_rem = false;
    } else {
        std::vector<idx_t>    lrow;
        std::vector<column_t> lcol;
        std::vector<real>     lval;

        std::vector<idx_t>    rrow;
        std::vector<column_t> rcol;
        std::vector<real>     rval;

        lrow.reserve(end - beg + 1);
        lrow.push_back(0);

        lcol.reserve(row[end] - row[beg]);
        lval.reserve(row[end] - row[beg]);

        if (!remote_cols.empty()) {
            rrow.reserve(end - beg + 1);
            rrow.push_back(0);

            rcol.reserve(row[end] - row[beg]);
            rval.reserve(row[end] - row[beg]);
        }

        // Renumber columns.
        std::unordered_map<column_t,column_t> r2l(2 * remote_cols.size());
        for(auto c = remote_cols.begin(); c != remote_cols.end(); c++) {
            size_t idx = r2l.size();
            r2l[*c] = idx;
        }

        for(size_t i = beg; i < end; i++) {
            for(idx_t j = row[i]; j < row[i + 1]; j++) {
                if (col[j] >= xbeg && col[j] < xend) {
                    lcol.push_back(col[j] - xbeg);
                    lval.push_back(val[j]);
                } else {
                    assert(r2l.count(col[j]));
                    rcol.push_back(r2l[col[j]]);
                    rval.push_back(val[j]);
                }
            }

            lrow.push_back(lcol.size());
            rrow.push_back(rcol.size());
        }

        cl::Event event;

        // Copy local part to the device.
        if (lrow.back()) {
            loc.row = cl::Buffer(
                    context, CL_MEM_READ_ONLY, lrow.size() * sizeof(idx_t));

            queue.enqueueWriteBuffer(
                    loc.row, CL_FALSE, 0, lrow.size() * sizeof(idx_t), lrow.data());

            loc.col = cl::Buffer(
                    context, CL_MEM_READ_ONLY, lcol.size() * sizeof(column_t));

            loc.val = cl::Buffer(
                    context, CL_MEM_READ_ONLY, lval.size() * sizeof(real));

            queue.enqueueWriteBuffer(
                    loc.col, CL_FALSE, 0, lcol.size() * sizeof(column_t), lcol.data());

            queue.enqueueWriteBuffer(
                    loc.val, CL_FALSE, 0, lval.size() * sizeof(real), lval.data(),
                    0, &event);
        }

        // Copy remote part to the device.
        if (!remote_cols.empty()) {
            rem.row = cl::Buffer(
                    context, CL_MEM_READ_ONLY, rrow.size() * sizeof(idx_t));

            rem.col = cl::Buffer(
                    context, CL_MEM_READ_ONLY, rcol.size() * sizeof(column_t));

            rem.val = cl::Buffer(
                    context, CL_MEM_READ_ONLY, rval.size() * sizeof(real));

            queue.enqueueWriteBuffer(
                    rem.row, CL_FALSE, 0, rrow.size() * sizeof(idx_t), rrow.data());

            queue.enqueueWriteBuffer(
                    rem.col, CL_FALSE, 0, rcol.size() * sizeof(column_t), rcol.data());

            queue.enqueueWriteBuffer(
                    rem.val, CL_FALSE, 0, rval.size() * sizeof(real), rval.data(),
                    0, &event);
        }

        if (lrow.back() || !remote_cols.empty()) event.wait();

        has_loc = lrow.back();
        has_rem = !remote_cols.empty();
    }
}

template <typename real, typename column_t, typename idx_t>
const kernel_cache_entry& SpMat<real,column_t,idx_t>::SpMatCSR::spmv_set(const cl::CommandQueue &queue) {
    static kernel_cache cache;

    cl::Context context = qctx(queue);
    cl::Device  device  = qdev(queue);

    auto kernel = cache.find(context());

    if (kernel == cache.end()) {
        std::ostringstream source;

        source << standard_kernel_header(device) <<
            "typedef " << type_name<real>() << " real;\n"
            "kernel void spmv_set(\n"
            "    " << type_name<size_t>() << " n,\n"
            "    global const " << type_name<idx_t>() << " *row,\n"
            "    global const " << type_name<column_t>() << " *col,\n"
            "    global const real *val,\n"
            "    global const real *x,\n"
            "    global real *y,\n"
            "    real alpha\n"
            "    )\n"
            "{\n"
            "    size_t chunk_size  = (n + get_global_size(0) - 1) / get_global_size(0);\n"
            "    size_t chunk_start = get_global_id(0) * chunk_size;\n"
            "    size_t chunk_end   = min(n, chunk_start + chunk_size);\n"
            "    for (size_t i = chunk_start; i < chunk_end; ++i) {\n"
            "        real sum = 0;\n"
            "        size_t beg = row[i];\n"
            "        size_t end = row[i + 1];\n"
            "        for(size_t j = beg; j < end; j++)\n"
            "            sum += val[j] * x[col[j]];\n"
            "        y[i] = alpha * sum;\n"
            "    }\n"
            "}\n";

        auto program = build_sources(context, source.str());

        cl::Kernel krn(program, "spmv_set");
        size_t     wgs = kernel_workgroup_size(krn, device);

        kernel = cache.insert(std::make_pair(
                    context(), kernel_cache_entry(krn, wgs)
                    )).first;
    }

    return kernel->second;
}

template <typename real, typename column_t, typename idx_t>
const kernel_cache_entry& SpMat<real,column_t,idx_t>::SpMatCSR::spmv_add(const cl::CommandQueue &queue) {
    static kernel_cache cache;

    cl::Context context = qctx(queue);
    cl::Device  device  = qdev(queue);

    auto kernel = cache.find(context());

    if (kernel == cache.end()) {
        std::ostringstream source;

        source << standard_kernel_header(device) <<
            "typedef " << type_name<real>() << " real;\n"
            "kernel void spmv_add(\n"
            "    " << type_name<size_t>() << " n,\n"
            "    global const " << type_name<idx_t>() << " *row,\n"
            "    global const " << type_name<column_t>() << " *col,\n"
            "    global const real *val,\n"
            "    global const real *x,\n"
            "    global real *y,\n"
            "    real alpha\n"
            "    )\n"
            "{\n"
            "    size_t chunk_size  = (n + get_global_size(0) - 1) / get_global_size(0);\n"
            "    size_t chunk_start = get_global_id(0) * chunk_size;\n"
            "    size_t chunk_end   = min(n, chunk_start + chunk_size);\n"
            "    for (size_t i = chunk_start; i < chunk_end; ++i) {\n"
            "        real sum = 0;\n"
            "        size_t beg = row[i];\n"
            "        size_t end = row[i + 1];\n"
            "        for(size_t j = beg; j < end; j++)\n"
            "            sum += val[j] * x[col[j]];\n"
            "        y[i] += alpha * sum;\n"
            "    }\n"
            "}\n";

        auto program = build_sources(context, source.str());

        cl::Kernel krn(program, "spmv_add");
        size_t     wgs = kernel_workgroup_size(krn, device);

        kernel = cache.insert(std::make_pair(
                    context(), kernel_cache_entry(krn, wgs)
                    )).first;
    }

    return kernel->second;
}

template <typename real, typename column_t, typename idx_t>
void SpMat<real,column_t,idx_t>::SpMatCSR::mul_local(
        const cl::Buffer &x, const cl::Buffer &y,
        real alpha, bool append
        ) const
{
    cl::Context context = qctx(queue);
    cl::Device  device  = qdev(queue);

    if (has_loc) {
        if (append) {
            auto add = spmv_add(queue);
            size_t g_size = num_workgroups(device) * add.wgsize;

            uint pos = 0;
            add.kernel.setArg(pos++, n);
            add.kernel.setArg(pos++, loc.row);
            add.kernel.setArg(pos++, loc.col);
            add.kernel.setArg(pos++, loc.val);
            add.kernel.setArg(pos++, x);
            add.kernel.setArg(pos++, y);
            add.kernel.setArg(pos++, alpha);

            queue.enqueueNDRangeKernel(add.kernel, cl::NullRange, g_size, add.wgsize);
        } else {
            auto set = spmv_set(queue);
            size_t g_size = num_workgroups(device) * set.wgsize;

            uint pos = 0;
            set.kernel.setArg(pos++, n);
            set.kernel.setArg(pos++, loc.row);
            set.kernel.setArg(pos++, loc.col);
            set.kernel.setArg(pos++, loc.val);
            set.kernel.setArg(pos++, x);
            set.kernel.setArg(pos++, y);
            set.kernel.setArg(pos++, alpha);

            queue.enqueueNDRangeKernel(set.kernel, cl::NullRange, g_size, set.wgsize);
        }
    } else if (!append) {
        vector<real>(queue, y) = 0;
    }
}

template <typename real, typename column_t, typename idx_t>
void SpMat<real,column_t,idx_t>::SpMatCSR::mul_remote(
        const cl::Buffer &x, const cl::Buffer &y,
        real alpha, const std::vector<cl::Event> &event
        ) const
{
    if (!has_rem) return;

    cl::Context context = qctx(queue);
    cl::Device  device  = qdev(queue);

    auto add = spmv_add(queue);
    size_t g_size = num_workgroups(device) * add.wgsize;

    uint pos = 0;
    add.kernel.setArg(pos++, n);
    add.kernel.setArg(pos++, rem.row);
    add.kernel.setArg(pos++, rem.col);
    add.kernel.setArg(pos++, rem.val);
    add.kernel.setArg(pos++, x);
    add.kernel.setArg(pos++, y);
    add.kernel.setArg(pos++, alpha);

    queue.enqueueNDRangeKernel(add.kernel, cl::NullRange, g_size, add.wgsize, &event);
}

/// Weights device wrt to spmv performance.
/**
 * Launches the following kernel on each device:
 * \code
 * y = A * x;
 * \endcode
 * where x and y are vectors, and A is matrix for 3D Poisson problem in square
 * domain. Each device gets portion of the vector proportional to the
 * performance of this operation.
 */
inline double device_spmv_perf(const cl::CommandQueue &q) {
    static const size_t test_size = 64U;

    std::vector<cl::CommandQueue> queue(1, q);

    // Construct matrix for 3D Poisson problem in cubic domain.
    const size_t n   = test_size;
    const float  h2i = (n - 1.0f) * (n - 1.0f);

    std::vector<size_t> row;
    std::vector<size_t> col;
    std::vector<float>  val;

    row.reserve(n * n * n + 1);
    col.reserve(6 * (n - 2) * (n - 2) * (n - 2) + n * n * n);
    val.reserve(6 * (n - 2) * (n - 2) * (n - 2) + n * n * n);

    row.push_back(0);
    for(size_t k = 0, idx = 0; k < n; k++) {
        for(size_t j = 0; j < n; j++) {
            for(size_t i = 0; i < n; i++, idx++) {
                if (
                        i == 0 || i == (n - 1) ||
                        j == 0 || j == (n - 1) ||
                        k == 0 || k == (n - 1)
                   )
                {
                    col.push_back(idx);
                    val.push_back(1);
                    row.push_back(row.back() + 1);
                } else {
                    col.push_back(idx - n * n);
                    val.push_back(-h2i);

                    col.push_back(idx - n);
                    val.push_back(-h2i);

                    col.push_back(idx - 1);
                    val.push_back(-h2i);

                    col.push_back(idx);
                    val.push_back(6 * h2i);

                    col.push_back(idx + 1);
                    val.push_back(-h2i);

                    col.push_back(idx + n);
                    val.push_back(-h2i);

                    col.push_back(idx + n * n);
                    val.push_back(-h2i);

                    row.push_back(row.back() + 7);
                }
            }
        }
    }

    // Create device vectors and copy of the matrix.
    size_t n3 = n * n * n;
    vex::SpMat<float>  A(queue, n3, n3, row.data(), col.data(), val.data());
    vex::vector<float> x(queue, n3);
    vex::vector<float> y(queue, n3);

    // Warming run.
    x = 1;
    A.mul(x, y);

    // Measure performance.
    profiler<> prof(queue);
    prof.tic_cl("");
    A.mul(x, y);
    double time = prof.toc("");
    return 1.0 / time;
}

} // namespace vex

#include <vexcl/spmat/ccsr.hpp>

#ifdef WIN32
#  pragma warning(pop)
#endif

// vim: et
#endif
