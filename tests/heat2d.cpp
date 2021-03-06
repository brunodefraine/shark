/*
 * Copyright (c) 2010-2013, Vrije Universiteit Brussel.
 * Copyright (c) 2014-2015, imec
 * All rights reserved.
 */

#include <cmath>
#include <iostream>
#include <sstream>
#include <getopt.h>
#include <shark.hpp>

using namespace std;
using namespace shark;
using namespace shark::types2d;

void init_pyramid(GlobalArrayD& ga)
{
	const coords_range outer = { ga.domain().total().lower - ga.ghost_width(), ga.domain().total().upper + ga.ghost_width() };
	const vecD one = {{1.0, 1.0}};
	const vecD mid = {{0.5, 0.5}};

	ga = 1.0 - max_element(abs(coord_vec(ga.domain(), outer, one) - mid)) / 0.5;
}

void heat(const GlobalArrayD& ga, GlobalArrayD& gb, double nu)
{
	// Sync and make sure ga is complete
	ga.update();

#if 1
	gb = unary(ga, [nu](const AccessD& u, coords ii) -> double {
		coords left;   left[0] = ii[0]-1;  left[1] = ii[1];
		coords right; right[0] = ii[0]+1; right[1] = ii[1];
		coords above; above[0] = ii[0];   above[1] = ii[1]-1;
		coords below; below[0] = ii[0];   below[1] = ii[1]+1;

		return u(ii) + nu * (u(left) + u(right) + u(above) + u(below) - 4 * u(ii));
	});
#else
	coords west;   west[0] = -1; west[1] = 0;
	coords east;   east[0] = 1;  east[1] = 0;
	coords north; north[0] = 0; north[1] = -1;
	coords south; south[0] = 0; south[1] = 1;

	gb = ga + nu * (shift(ga,west) + shift(ga,east) + shift(ga,north) + shift(ga,south) - 4 * ga);
#endif
}

void heat_loop(int n, GlobalArrayD& ga, GlobalArrayD& gb, int nr, double dt) {
	double nu = dt * n * n;

	for(int k = 0; k < nr; k++)
	{
		if (k % 2 == 0)
			heat(ga, gb, nu);
		else
			heat(gb, ga, nu);
	}
}

int main(int argc, char **argv)
{
	Init(&argc, &argv);

	//Default values that might be overridden by options
	int nr = 2000;
	int n = 511;	//incremented by one internally
	bool block = false;

	//Process arguments
	int ch;

	while((ch = getopt(argc, argv, "n:i:t:bh")) != -1)
	{
		switch(ch)
		{
			case 'n':
				{
					istringstream iss(optarg);
					iss >> n;
				}
				break;
			case 'i':
				{
					istringstream iss(optarg);
					iss >> nr; 
				}
				break;
			case 't':
				{
					istringstream iss(optarg);
					iss >> nthrds;
				}
				break;
			case 'b':
				block = true;
				break;
			case 'h':
			case '?':
			default:
				cerr << "Usage: " << argv[0] << " [-n <grid-cells>] [-i <iterations>] [-t <threads>]\n" << endl;
				Abort(1);
		}
	}

	if(world().procid == 0)
	{
		cerr << "sched: " << sched << endl;
		cerr << "n: " << n << endl;
		cerr << "nr: " << nr << endl;
		cerr << "block: " << boolalpha << block << noboolalpha << endl;
		cerr << "nprocs: " << world().nprocs << endl;
		cerr << "nthrds: " << nthrds << endl;
	}

	//Declare auxiliary variables and run	
	double dt = 0.25 / n / n;
	SetupThreads();

	{
		const coords size  = {{n-1,n-1}};
		const coords ghost = {{1,1}};

		const array<int,2> pcoords = {{ 0, block ? 0 : 1 }};
		Domain d(world(), size, pcoords);

		if(world().procid == 0)
			d.outputDistribution(cerr);

		typename GlobalArrayD::bounds bd = {{ BoundaryD::constant(0.0), BoundaryD::constant(0.0) }};

		GlobalArrayD ga(d, ghost, false, bd);
		GlobalArrayD gb(d, ghost, false, bd);
		world().sync();

		init_pyramid(ga);

		double e0 = norm1(ga) / n / n;
		double f0 = norm2(ga) / n / n;

		if(world().procid == 0)
		{
			cerr << "e0: " << e0 << endl;
			cerr << "f0: " << f0 << endl;
		}

		double starttime = Wtime();
		heat_loop(n, ga, gb, nr, dt);
		double endtime = Wtime();
		double e1 = norm1(ga) / n / n;
		double f1 = norm2(ga) / n / n;

		if(world().procid == 0)
		{
			cerr << "e1: " << e1 << endl;
			cerr << "f1: " << f1 << endl;
			cerr << "t: " << dt * nr << endl;
			cerr << "runtime: " << endtime - starttime << endl;
		}
	}

	Finalize();

	return 0;
}

