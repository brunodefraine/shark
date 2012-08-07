
#include <iostream>
#include <shark.hpp>

using namespace std;
using namespace shark;
using namespace shark::types3d;

int main(int argc, char* argv[]) {
	Init(&argc, &argv);
	SetupThreads();

	cerr << "Hello world from " << world().procid << " of " << world().nprocs << endl;
	world().sync();
	{
		coords c = {{100,100,100}};
		Domain dom(world(), c);
		if(world().procid == 0) {
			dom.outputDistribution(cerr);
			for(int k = 0; k < world().nprocs; k++) {
				coords c = dom.count(k);
				cerr << "Process " << k << " has data " << dom.hasData(k) << ": " << c << endl;
				typename Domain::pcoords ip = dom.indexp(k);
				cerr << "Process " << k << "(" << dom.pindex(ip) << ")" << " has indices " << ip[0] << "," << ip[1] << "," << ip[2] << endl;

			}
		}

		coords gw = {{2,2,2}};
		GlobalArrayD ga(dom, gw);
		{
			AccessD a(ga);
			const GlobalArrayD& gac(ga);
			const AccessD b(gac);
		}
		ga = constant(dom, 0.0);
		coords_range inner = {gw,c-gw};
		{
			AccessD a(ga);
			Region(dom,inner).for_each([&a](coords i) {
					a(i) = 1.23;
			});
		}
		GlobalArrayD gb(dom);
		gb = 2 * ga + (-ga) + ga - ga;
		gb = gb * gb;
		{
			const AccessD b(gb);
			Region(dom,inner).for_each([&b](coords i) {
					if(b(i) != 1.23 * 1.23)
						cout << i << ": " << b(i) << endl;
			});
		}
	}

	Finalize();
}

