
#include <shark/access.hpp>
#include <shark/domain.hpp>
#include <shark/globalarray.hpp>
#include <shark/sparsearray.hpp>
#include "comm_impl.hpp"

// Complete types
namespace shark {
	namespace ndim {
		template<int ndim, typename T>
		class Boundary<ndim,T>::type {
		public:
			virtual ~type();
			virtual type* clone() const;
		};

		template<int ndim, typename T>
		class Boundary<ndim,T>::periodic_type: public type {
		public:
			periodic_type();
			virtual ~periodic_type();
			virtual periodic_type* clone() const;
		};
	}
}

using namespace std;
using namespace shark;
using namespace shark::ndim;

template<int ndim, typename T>
Boundary<ndim,T>::type::~type() { }

template<int ndim, typename T>
typename Boundary<ndim,T>::type* Boundary<ndim,T>::type::clone() const {
	return new type();
}

template<int ndim, typename T>
Boundary<ndim,T>::periodic_type::periodic_type() { }

template<int ndim, typename T>
Boundary<ndim,T>::periodic_type::~periodic_type() { }

template<int ndim, typename T>
typename Boundary<ndim,T>::periodic_type* Boundary<ndim,T>::periodic_type::clone() const {
	return new periodic_type();
}

template<int ndim, typename T>
Boundary<ndim,T>::Boundary(type* t): t(t) { }

template<int ndim, typename T>
Boundary<ndim,T>::Boundary(): t(new type()) { }

template<int ndim, typename T>
Boundary<ndim,T>::~Boundary() { }

template<int ndim, typename T>
Boundary<ndim,T>::Boundary(const Boundary<ndim,T>& other): t(other.t->clone()) { }

template<int ndim, typename T>
Boundary<ndim,T>& Boundary<ndim,T>::operator=(const Boundary<ndim,T>& other) {
	t.reset(other.t->clone());
	return *this;
}

template<int ndim, typename T>
Boundary<ndim,T> Boundary<ndim,T>::periodic() {
	return Boundary<ndim,T>(new periodic_type());
}

template<int ndim, typename T>
const int GlobalArray<ndim,T>::number_of_dimensions;

template<int ndim, typename T>
GlobalArray<ndim,T>::GlobalArray(): dom(0) { }

template<int ndim, typename T>
GlobalArray<ndim,T>::~GlobalArray() {
	if(*this) {
		assert(lc == 0);
		deallocate();
	}
}

template<int ndim, typename T>
GlobalArray<ndim,T>::GlobalArray(GlobalArray<ndim,T>&& other):
	dom(other.dom),
	gw(other.gw),
	gc(other.gc),
	bd(std::move(other.bd)),
	ptr(other.ptr),
	impl(std::move(other.impl)),
	ld(std::move(other.ld)),
	ghost_back(std::move(other.ghost_back)),
	ghost_front(std::move(other.ghost_front)),
	lc(0)
{
	assert(!other || other.lc == 0);
	other.reset();
}

template<int ndim, typename T>
GlobalArray<ndim,T>& GlobalArray<ndim,T>::operator=(GlobalArray<ndim,T>&& other) {
	assert(!other || other.lc == 0);
	if(*this)
		deallocate();
	dom = other.dom;
	gw = other.gw;
	gc = other.gc;
	bd = std::move(other.bd);
	ptr = other.ptr;
	impl = std::move(other.impl);
	ld = std::move(other.ld);
	ghost_back = std::move(other.ghost_back);
	ghost_front = std::move(other.ghost_front);
	lc = 0;
	other.reset();
	return *this;
}

template<int ndim, typename T>
GlobalArray<ndim,T>& GlobalArray<ndim,T>::operator=(const GlobalArray<ndim,T>& other) {
	region(domain().total()) = other;
	return *this;
}

template <int ndim, typename T>
void GlobalArray<ndim,T>::reset() {
	dom = 0;
}

template<int ndim,typename T>
coords<ndim+1> GlobalArray<ndim,T>::stride(coords<ndim> count, coords<ndim> ghost_width) {
	coords<ndim+1> ld;
	ld[ndim] = 1;
	for(int d = ndim-1; d >= 0; d--)
		ld[d] = ld[d+1] * (count[d] + 2 * ghost_width[d]);
	return ld;
}

template<int ndim,typename T>
void GlobalArray<ndim,T>::allocate() {
	const coords<ndim> count = domain().count();
	const coords<ndim> gw = ghost_width();
	const bool gc = ghost_corners(); 

	ld = stride(count, gw);

	for(int di = 0; di < ndim; di++)
		for(int d = 0; d < ndim; d++)
			if(d == di) {
				ghost_back [di].lower[d] = -gw[d];
				ghost_back [di].upper[d] = 0;
				ghost_front[di].lower[d] = count[d];
				ghost_front[di].upper[d] = count[d] + gw[d];
			} else if(gc && d < di) {
				ghost_back [di].lower[d] = -gw[d];
				ghost_back [di].upper[d] = count[d] + gw[d];
				ghost_front[di].lower[d] = -gw[d];
				ghost_front[di].upper[d] = count[d] + gw[d];
			} else {
				ghost_back [di].lower[d] = 0;
				ghost_back [di].upper[d] = count[d];
				ghost_front[di].lower[d] = 0;
				ghost_front[di].upper[d] = count[d];
			}

#if defined(SHARK_MPI_COMM)
	{
		// Create ghost types
		MPI_Datatype base = mpi_type<T>::t;
		if(mpi_type<T>::count() != 1)
			MPI_Type_contiguous(mpi_type<T>::count(), base, &base);
		MPI_Aint lb, extent;
		MPI_Type_get_extent(base, &lb, &extent);
		for(int di = 0; di < ndim; di++) {
			MPI_Type_dup(base, &impl->ghost[di]);
			for(int d = ndim-1; d >= 0; d--) {
				MPI_Datatype tmp = impl->ghost[di];
				coord n = ghost_back[di].upper[d] - ghost_back[di].lower[d];
				MPI_Type_create_hvector(n, 1, ld[d+1]*extent, tmp, &impl->ghost[di]);
				MPI_Type_free(&tmp);
			}
			MPI_Type_commit(&impl->ghost[di]);
		}
		if(mpi_type<T>::count() != 1)
			MPI_Type_free(&base);
	}

	// Allocate memory
	MPI_Aint size = ld[0] * sizeof(T);
	MPI_Alloc_mem(size, MPI_INFO_NULL, &ptr);

	// Create window
	MPI_Win_create(ptr, size, static_cast<int>(sizeof(T)), MPI_INFO_NULL, domain().group.impl->comm, &impl->win);
#elif defined(SHARK_NO_COMM)
	unused(impl);
	ptr = static_cast<T*>(mem_alloc(ld[0] * sizeof(T)));
#else
#error "No comm allocate"
#endif
}

// Constructors
template<int ndim, typename T>
GlobalArray<ndim,T>::GlobalArray(const Domain<ndim>& domain, coords<ndim> ghost_width, bool ghost_corners, bounds bd):
  dom(&domain), gw(ghost_width), gc(ghost_corners), bd(bd), impl(new GAImpl<ndim>()), lc(0) {
	allocate();
}

template<int ndim, typename T>
GlobalArray<ndim,T>::GlobalArray(const GlobalArray<ndim,T>& other, bool copy):
  dom(other.dom), gw(other.gw), gc(other.gc), bd(other.bd), impl(new GAImpl<ndim>()), lc(0) {
	allocate();

	if(copy)
		*this = other;
}

// Destructor

template<int ndim,typename T>
void GlobalArray<ndim,T>::deallocate() {
#if defined(SHARK_MPI_COMM)
	for(int di = 0; di < ndim; di++)
		MPI_Type_free(&impl->ghost[di]);

	MPI_Win_free(&impl->win);
	MPI_Free_mem(ptr);
#elif defined(SHARK_NO_COMM)
	mem_free(ptr);
#else
#error "No comm deallocate"
#endif
}

template<int ndim,typename T>
void GlobalArray<ndim,T>::reshape(const Domain<ndim>& domain) {
	assert(this->domain().equiv(domain));

	GlobalArray<ndim,T> tmp(domain, gw, gc, bd);

	if(domain.hasData()) {
		array<size_t,ndim-1> eld;
		size_t off = gw[ndim-1];
		for(int d = ndim-2; d >= 0; d--) {
			eld[d] = tmp.ld[d+1];
			off += gw[d] * eld[d];
		}
		this->get(domain.local(), eld, tmp.ptr+off);
	}

	*this = move(tmp);
}

// Inspectors

template<int ndim,typename T>
void GlobalArray<ndim,T>::update() const {
	const coords<ndim> gw = ghost_width();
#if defined(SHARK_MPI_COMM)
	const bool gc = ghost_corners();
	MPI_Comm comm = domain().group.impl->comm;
	MPI_Request req[4*ndim];

	for(int di = 0; di < ndim; di++) {
		if(gw[di] > 0) {
			bool pd = dynamic_cast<typename Boundary<ndim,T>::periodic_type*>(bd[di].t.get()) != nullptr;
			int prev = domain().shiftd(di, -1, pd);
			int next = domain().shiftd(di,  1, pd);
			// backward
			MPI_Isend(&da(ghost_back[di].adj(di, 1)), 1, impl->ghost[di], prev, 2*di, comm, &req[4*di]);
			MPI_Irecv(&da(ghost_front[di].lower), 1, impl->ghost[di], next, 2*di, comm, &req[4*di+1]);
			// forward
			MPI_Isend(&da(ghost_front[di].adj(di, -1)), 1, impl->ghost[di], next, 2*di+1, comm, &req[4*di+2]);
			MPI_Irecv(&da(ghost_back[di].lower), 1, impl->ghost[di], prev, 2*di+1, comm, &req[4*di+3]);
		} else {
			req[4*di] = MPI_REQUEST_NULL;
			req[4*di+1] = MPI_REQUEST_NULL;
			req[4*di+2] = MPI_REQUEST_NULL;
			req[4*di+3] = MPI_REQUEST_NULL;
		}
		if(gc)
			MPI_Waitall(4, &req[4*di], MPI_STATUSES_IGNORE);
	}
	if(!gc)
		MPI_Waitall(4*ndim, req, MPI_STATUSES_IGNORE);

#elif defined(SHARK_NO_COMM)
	const coords<ndim> count = domain().count();

	for(int di = 0; di < ndim; di++) {
		if(gw[di] > 0 && dynamic_cast<typename Boundary<ndim,T>::periodic_type*>(bd[di].t.get()) != nullptr) {
			coords<ndim> off;
			for(int d = 0; d < ndim; d++) {
				off[d] = d == di ? count[di] : 0;
			}
			ghost_back[di].for_each([this,&off](coords<ndim> ii) {
				this->da(ii) = this->da(ii+off);
			});
			for(int d = 0; d < ndim; d++) {
				off[d] = d == di ? -count[di] : 0;
			}
			ghost_front[di].for_each([this,&off](coords<ndim> ii) {
				this->da(ii) = this->da(ii+off);
			});
		}
}

#else
#error "No comm update"
#endif
}

#if defined(SHARK_MPI_COMM)

template<int ndim,typename T>
class GlobalArray<ndim,T>::RMAOp {
	typedef array<int,ndim> pcoords;

	const Domain<ndim>& dom;
	const coords_range<ndim> range;
	const coords<ndim> gw;
	const array<size_t,ndim-1> ognld;
	pcoords lip, uip;
	coords<ndim> loff, uoff;

	template<int d,typename Op>
	INLINE typename enable_if<d < ndim>::type opd(const Op& op, pcoords& ip, coords_range<ndim>& i);

	template<int d,typename Op>
	INLINE typename enable_if<d == ndim>::type opd(const Op& op, pcoords& ip, coords_range<ndim>& i);

public:
	RMAOp(const Domain<ndim>& dom, coords_range<ndim> range, coords<ndim> gw, array<size_t,ndim-1> ognld);

	template<typename Op>
	void op(Op op);
};

template<int ndim,typename T> template<int d,typename Op>
inline typename enable_if<d < ndim>::type GlobalArray<ndim,T>::RMAOp::opd(const Op& op, pcoords& ip, coords_range<ndim>& i) {
	for(ip[d] = lip[d]; ip[d] <= uip[d]; ip[d]++) {
		i.lower[d] = ip[d] == lip[d] ? loff[d] : 0;
		i.upper[d] = ip[d] == uip[d] ? uoff[d] : dom.nd[d][ip[d]+1] - dom.nd[d][ip[d]];
		opd<d+1>(op, ip, i);
	}
}

template<int ndim,typename T> template<int d,typename Op>
inline typename enable_if<d == ndim>::type GlobalArray<ndim,T>::RMAOp::opd(const Op& op, pcoords& ip, coords_range<ndim>& i) {
	// Target
	int id = dom.pindex(ip);
	coords<ndim+1> tgtld = stride(dom.count(id), gw);

	// Offsets
	size_t ognoff = 0;
	MPI_Aint tgtoff = 0;
	for(int di = 0; di < ndim; di++) {
		ognoff += (di == ndim-1 ? 1 : ognld[di]) * (dom.nd[di][ip[di]] + i.lower[di] - range.lower[di]);
		tgtoff += tgtld[di+1] * (gw[di] + i.lower[di]);
	}

	// Types
	MPI_Datatype ognt, tgtt;
	{
		MPI_Aint lb, extent;
		MPI_Type_get_extent(mpi_type<T>::t, &lb, &extent);

		MPI_Type_dup(mpi_type<T>::t, &ognt);
		MPI_Type_dup(mpi_type<T>::t, &tgtt);
		for(int di = ndim-1; di >= 0; di--) {
			MPI_Datatype tmp;
			coord n = i.upper[di] - i.lower[di];
			tmp = ognt;
			MPI_Type_create_hvector(n, 1, di == ndim-1 ? extent : ognld[di]*extent, tmp, &ognt);
			MPI_Type_free(&tmp);
			tmp = tgtt;
			MPI_Type_create_hvector(n, 1, tgtld[di+1]*extent, tmp, &tgtt);
			MPI_Type_free(&tmp);
		}
		MPI_Type_commit(&ognt);
		MPI_Type_commit(&tgtt);
	}
	op(id, ognoff, ognt, tgtoff, tgtt);

	MPI_Type_free(&tgtt);
	MPI_Type_free(&ognt);
}

template<int ndim,typename T>
GlobalArray<ndim,T>::RMAOp::RMAOp(const Domain<ndim>& dom, coords_range<ndim> range, coords<ndim> gw, array<size_t,ndim-1> ognld)
: dom(dom), range(range), gw(gw), ognld(ognld) {
	assert(range.lower >= coords<ndim>() && range.lower < dom.n);
	assert(range.upper >= coords<ndim>() && range.upper <= dom.n);
	assert(range.lower <= range.upper);
	dom.find(range.lower, lip, loff);
	dom.find(range.upper, uip, uoff);
	for(int d = 0; d < ndim; d++)
		if(uoff[d] == 0) {
			uip[d]--;
			uoff[d] = dom.nd[d][uip[d]+1] - dom.nd[d][uip[d]];
		}
}

template<int ndim,typename T> template<typename Op>
void GlobalArray<ndim,T>::RMAOp::op(Op op) {
	pcoords ip;
	coords_range<ndim> i;
	opd<0>(op, ip, i);
}

#endif

template<int ndim,typename T>
void GlobalArray<ndim,T>::get(coords_range<ndim> range, array<size_t,ndim-1> ld, T* buf) const {
#if defined(SHARK_MPI_COMM)
	RMAOp(domain(), range, ghost_width(), ld).op(
		[this,buf](int id, size_t ognoff, MPI_Datatype ognt, MPI_Aint tgtoff, MPI_Datatype tgtt) {
			MPI_Win_lock(MPI_LOCK_SHARED, id, 0, impl->win);
			MPI_Get(buf+ognoff, 1, ognt, id, tgtoff, 1, tgtt, impl->win);
			MPI_Win_unlock(id, impl->win);
		}
	);
#elif defined(SHARK_NO_COMM)
	range.for_each([this,&buf,&range,&ld](coords<ndim> i) {
		buf[(i - range.lower).offset(ld)] = this->da(i);
	});
#else
#error "No comm get"
#endif
}

template<int ndim,typename T>
void GlobalArray<ndim,T>::put(coords_range<ndim> range, array<size_t,ndim-1> ld, const T* buf) {
#if defined(SHARK_MPI_COMM)
	RMAOp(domain(), range, ghost_width(), ld).op(
		[this,buf](int id, size_t ognoff, MPI_Datatype ognt, MPI_Aint tgtoff, MPI_Datatype tgtt) {
			MPI_Win_lock(MPI_LOCK_EXCLUSIVE, id, 0, impl->win);
			MPI_Put(const_cast<T*>(buf+ognoff), 1, ognt, id, tgtoff, 1, tgtt, impl->win);
			MPI_Win_unlock(id, impl->win);
		}
	);
#elif defined(SHARK_NO_COMM)
	range.for_each([this,&buf,&range,&ld](coords<ndim> i) {
		this->da(i) = buf[(i - range.lower).offset(ld)];
	});
#else
#error "No comm put"
#endif
}

template<int ndim,typename T> template<typename>
void GlobalArray<ndim,T>::accumulate(coords_range<ndim> range, array<size_t,ndim-1> ld, const T* buf) {
#if defined(SHARK_MPI_COMM)
	RMAOp(domain(), range, ghost_width(), ld).op(
		[this,buf](int id, size_t ognoff, MPI_Datatype ognt, MPI_Aint tgtoff, MPI_Datatype tgtt) {
			MPI_Win_lock(MPI_LOCK_SHARED, id, 0, impl->win);
			MPI_Accumulate(const_cast<T*>(buf+ognoff), 1, ognt, id, tgtoff, 1, tgtt, MPI_SUM, impl->win);
			MPI_Win_unlock(id, impl->win);
		}
	);
#elif defined(SHARK_NO_COMM)
	range.for_each([this,&buf,&range,&ld](coords<ndim> i) {
		this->da(i) += buf[(i - range.lower).offset(ld)];
	});
#else
#error "No comm accumulate"
#endif
}

template<int ndim,typename T>
void GlobalArray<ndim,T>::gather(SparseArray<ndim,T>& sa) const {
	assert(domain() == sa.dom);
	auto eld = sa.eld();
	sa.iter([this,&sa,&eld](const coords_range<ndim>& r) {
		this->get(r, eld, sa.ptr + r.lower.offset(sa.ld));
	});
}

template<int ndim,typename T> template<typename>
void GlobalArray<ndim,T>::scatterAcc(const SparseArray<ndim,T>& sa) {
	assert(domain() == sa.dom);
	auto eld = sa.eld();
	sa.iter([this,&sa,&eld](const coords_range<ndim>& r) {
		this->accumulate(r, eld, sa.ptr + r.lower.offset(sa.ld));
	});
}

template<int ndim,typename T>
GADest<ndim,T> GlobalArray<ndim,T>::region(coords_range<ndim> r) {
	return GADest<ndim,T>(*this, r);
}

template<int ndim,typename T>
GADest<ndim,T>::GADest(GlobalArray<ndim,T>& ga, coords_range<ndim> r): ga(ga), r(r) { }

template<int ndim,typename T>
GADest<ndim,T>::~GADest() { }

template<int ndim,typename T>
GADest<ndim,T>::GADest(const GADest& gad): ga(gad.ga), r(gad.r) { }

template<int ndim, typename T>
GARef<ndim,T>::GARef(const GlobalArray<ndim,T>& ga): ga(ga) { }

template<int ndim, typename T>
GARef<ndim,T>::~GARef() { }

// Set-up instantiations

#include "types"

#define SYMBDT(d,T) template class shark::ndim::Boundary<d,T>; 
#include "inst_dimtype"
#undef SYMBDT

#define SYMBDT(d,T) template class shark::ndim::GlobalArray<d,T>; 
#include "inst_dimtype"
#undef SYMBDT

#define SYMB_ARITH

#define SYMBDT(d,T) template void shark::ndim::GlobalArray<d,T>::accumulate(coords_range<d>, std::array<std::size_t,d-1>, const T*);
#include "inst_dimtype"
#undef SYMBDT

#define SYMBDT(d,T) template void shark::ndim::GlobalArray<d,T>::scatterAcc(const SparseArray<d,T>&);
#include "inst_dimtype"
#undef SYMBDT

#undef SYMB_ARITH

#define SYMBDT(d,T) template class shark::ndim::GADest<d,T>; 
#include "inst_dimtype"
#undef SYMBDT
	
#define SYMBDT(d,T) template class shark::ndim::GARef<d,T>; 
#include "inst_dimtype"
#undef SYMBDT
