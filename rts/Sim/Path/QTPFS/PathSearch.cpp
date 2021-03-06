/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cassert>
#include <list>
#include <limits>

#include "PathSearch.hpp"
#include "Path.hpp"
#include "PathCache.hpp"
#include "NodeLayer.hpp"
#include "Sim/Misc/GlobalConstants.h"

#ifdef QTPFS_TRACE_PATH_SEARCHES
#include "Sim/Misc/GlobalSynced.h"
#endif

QTPFS::binary_heap<QTPFS::INode*> QTPFS::PathSearch::openNodes;



void QTPFS::PathSearch::Initialize(
	NodeLayer* layer,
	PathCache* cache,
	const float3& sourcePoint,
	const float3& targetPoint,
	const PathRectangle& searchArea
) {
	srcPoint = sourcePoint; srcPoint.ClampInBounds();
	tgtPoint = targetPoint; tgtPoint.ClampInBounds();
	curPoint = srcPoint;
	nxtPoint = tgtPoint;

	nodeLayer = layer;
	pathCache = cache;

	searchRect = searchArea;
	searchExec = NULL;

	srcNode = nodeLayer->GetNode(srcPoint.x / SQUARE_SIZE, srcPoint.z / SQUARE_SIZE);
	tgtNode = nodeLayer->GetNode(tgtPoint.x / SQUARE_SIZE, tgtPoint.z / SQUARE_SIZE);
	curNode = NULL;
	nxtNode = NULL;
	minNode = srcNode;
}

bool QTPFS::PathSearch::Execute(
	unsigned int searchStateOffset,
	unsigned int searchMagicNumber
) {
	searchState = searchStateOffset; // starts at NODE_STATE_OFFSET
	searchMagic = searchMagicNumber; // starts at numTerrainChanges

	haveFullPath = (srcNode == tgtNode);
	havePartPath = false;

	// early-out
	if (haveFullPath)
		return true;

	const bool srcBlocked = (srcNode->GetMoveCost() == QTPFS_POSITIVE_INFINITY);

	std::vector<INode*>& allNodes = nodeLayer->GetNodes();
	std::vector<INode*> ngbNodes;

	#ifdef QTPFS_TRACE_PATH_SEARCHES
	searchExec = new PathSearchTrace::Execution(gs->frameNum);
	#endif

	switch (searchType) {
		case PATH_SEARCH_ASTAR:    { hCostMult = 1.0f; } break;
		case PATH_SEARCH_DIJKSTRA: { hCostMult = 0.0f; } break;
		default:                   {    assert(false); } break;
	}

	// allow the search to start from an impassable node (because single
	// nodes can represent many terrain squares, some of which can still
	// be passable and allow a unit to move within a node)
	// NOTE: we need to make sure such paths do not have infinite cost!
	if (srcBlocked) {
		srcNode->SetMoveCost(0.0f);
	}

	{
		openNodes.reset();
		openNodes.push(srcNode);

		UpdateNode(srcNode, NULL, 0.0f, srcPoint.distance(tgtPoint), srcNode->GetMoveCost());
	}

	while (!openNodes.empty()) {
		Iterate(allNodes, ngbNodes);

		#ifdef QTPFS_TRACE_PATH_SEARCHES
		searchExec->AddIteration(searchIter);
		searchIter.Clear();
		#endif

		haveFullPath = (curNode == tgtNode);
		havePartPath = (minNode != srcNode);

		if (haveFullPath) {
			openNodes.reset();
		}
	}

	if (srcBlocked) {
		srcNode->SetMoveCost(QTPFS_POSITIVE_INFINITY);
	}
		

	#ifdef QTPFS_SUPPORT_PARTIAL_SEARCHES
	// adjust the target-point if we only got a partial result
	// NOTE:
	//   should adjust GMT::goalPos accordingly, otherwise
	//   units will end up spinning in-place over the last
	//   waypoint (since "atGoal" can never become true)
	if (!haveFullPath && havePartPath) {
		tgtNode    = minNode;
		tgtPoint.x = minNode->xmid() * SQUARE_SIZE;
		tgtPoint.z = minNode->zmid() * SQUARE_SIZE;
	}
	#endif

	return (haveFullPath || havePartPath);
}



void QTPFS::PathSearch::UpdateNode(
	INode* nxtNode,
	INode* curNode,
	float gCost,
	float hCost,
	float mCost
) {
	// NOTE:
	//   the heuristic must never over-estimate the distance,
	//   but this is *impossible* to achieve on a non-regular
	//   grid on which any node only has an average move-cost
	//   associated with it --> paths will be "nearly optimal"
	nxtNode->SetSearchState(searchState | NODE_STATE_OPEN);
	nxtNode->SetPrevNode(curNode);
	nxtNode->SetPathCost(NODE_PATH_COST_G, gCost                      );
	nxtNode->SetPathCost(NODE_PATH_COST_H,         (hCost * hCostMult));
	nxtNode->SetPathCost(NODE_PATH_COST_F, gCost + (hCost * hCostMult));
	nxtNode->SetPathCost(NODE_PATH_COST_M, mCost                      );

	#ifdef QTPFS_WEIGHTED_HEURISTIC_COST
	nxtNode->SetNumPrevNodes((curNode != NULL)? (curNode->GetNumPrevNodes() + 1): 0);
	#endif
}

void QTPFS::PathSearch::Iterate(
	const std::vector<INode*>& allNodes,
	      std::vector<INode*>& ngbNodes
) {
	curNode = openNodes.top();
	curNode->SetSearchState(searchState | NODE_STATE_CLOSED);
	#ifdef QTPFS_CONSERVATIVE_NEIGHBOR_CACHE_UPDATES
	// in the non-conservative case, this is done from
	// NodeLayer::ExecNodeNeighborCacheUpdates instead
	curNode->SetMagicNumber(searchMagic);
	#endif

	openNodes.pop();
	openNodes.check_heap_property(0);

	#ifdef QTPFS_TRACE_PATH_SEARCHES
	searchIter.SetPoppedNodeIdx(curNode->zmin() * gs->mapx + curNode->xmin());
	#endif

	if (curNode == tgtNode)
		return;
	if (curNode != srcNode)
		curPoint = curNode->GetNeighborEdgeTransitionPoint(curNode->GetPrevNode(), curPoint);
	if (curNode->GetMoveCost() == QTPFS_POSITIVE_INFINITY)
		return;

	if (curNode->xmid() < searchRect.x1) return;
	if (curNode->zmid() < searchRect.z1) return;
	if (curNode->xmid() > searchRect.x2) return;
	if (curNode->zmid() > searchRect.z2) return;

	#ifdef QTPFS_SUPPORT_PARTIAL_SEARCHES
	// remember the node with lowest h-cost in case the search fails to reach tgtNode
	if (curNode->GetPathCost(NODE_PATH_COST_H) < minNode->GetPathCost(NODE_PATH_COST_H))
		minNode = curNode;
	#endif


	#ifdef QTPFS_WEIGHTED_HEURISTIC_COST
	const float hWeight = math::sqrtf(curNode->GetPathCost(NODE_PATH_COST_M) / (curNode->GetNumPrevNodes() + 1));
	#else
	// the default speedmod on flat terrain (assuming no typemaps) is 1.0
	// this value lies halfway between the minimum and the maximum of the
	// speedmod range (2.0), so a node covering such terrain will receive
	// a *relative* (average) speedmod of 0.5 --> the average move-cost of
	// a "virtual node" containing nxtPoint and tgtPoint is the inverse of
	// 0.5, making our "admissable" heuristic distance-weight 2.0 (1.0/0.5)
	const float hWeight = 2.0f;
	#endif

	#ifdef QTPFS_COPY_ITERATE_NEIGHBOR_NODES
	const unsigned int numNgbs = curNode->GetNeighbors(allNodes, ngbNodes);
	#else
	// cannot assign to <ngbNodes> because that would still make a copy
	const std::vector<INode*>& nxtNodes = curNode->GetNeighbors(allNodes);
	const unsigned int numNgbs = nxtNodes.size();
	#endif

	for (unsigned int i = 0; i < numNgbs; i++) {
		// NOTE:
		//   this uses the actual distance that edges of the final path will cover,
		//   from <curPoint> (initialized to sourcePoint) to the middle of the edge
		//   shared between <curNode> and <nxtNode>
		//   (each individual path-segment is weighted by the average move-cost of
		//   the node it crosses; the heuristic is weighted by the average move-cost
		//   of all nodes encountered along partial path thus far)
		// NOTE:
		//   heading for the MIDDLE of the shared edge is not always the best option
		//   we deal with this sub-optimality later (in SmoothPath if it is enabled)
		// NOTE:
		//   short paths that should have 3 points (2 nodes) can contain 4 (3 nodes);
		//   this happens when a path takes a "detour" through a corner neighbor of
		//   srcNode if the shared corner vertex is closer to the goal position than
		//   the mid-point on the edge between srcNode and tgtNode
		// NOTE:
		//   H needs to be of the same order as G, otherwise the search reduces to
		//   Dijkstra (if G dominates H) or becomes inadmissable (if H dominates G)
		//   in the first case we would explore many more nodes than necessary (CPU
		//   nightmare), while in the second we would get low-quality paths (player
		//   nightmare)
		#ifdef QTPFS_COPY_ITERATE_NEIGHBOR_NODES
		nxtNode = ngbNodes[i];
		#else
		nxtNode = nxtNodes[i];
		#endif

		#ifdef QTPFS_CACHED_EDGE_TRANSITION_POINTS
		nxtPoint = curNode->GetNeighborEdgeTransitionPoint(i);
		#else
		nxtPoint = curNode->GetNeighborEdgeTransitionPoint(nxtNode, curPoint);
		#endif

		if (nxtNode->GetMoveCost() == QTPFS_POSITIVE_INFINITY)
			continue;

		const bool isCurrent = (nxtNode->GetSearchState() >= searchState);
		const bool isClosed = ((nxtNode->GetSearchState() & 1) == NODE_STATE_CLOSED);
		const bool isTarget = (nxtNode == tgtNode);

		// cannot use squared-distances because that will bias paths
		// towards smaller nodes (eg. 1^2 + 1^2 + 1^2 + 1^2 != 4^2)
		const float gDist = curPoint.distance(nxtPoint);
		const float hDist = nxtPoint.distance(tgtPoint);

		const float mCost =
			curNode->GetPathCost(NODE_PATH_COST_M) +
			curNode->GetMoveCost() +
			nxtNode->GetMoveCost() * int(isTarget);
		const float gCost =
			curNode->GetPathCost(NODE_PATH_COST_G) +
			curNode->GetMoveCost() * gDist +
			nxtNode->GetMoveCost() * hDist * int(isTarget);
		const float hCost = hWeight * hDist * int(!isTarget);

		if (!isCurrent) {
			UpdateNode(nxtNode, curNode, gCost, hCost, mCost);

			openNodes.push(nxtNode);
			openNodes.check_heap_property(0);

			#ifdef QTPFS_TRACE_PATH_SEARCHES
			searchIter.AddPushedNodeIdx(nxtNode->zmin() * gs->mapx + nxtNode->xmin());
			#endif

			continue;
		}

		if (gCost >= nxtNode->GetPathCost(NODE_PATH_COST_G))
			continue;
		if (isClosed)
			openNodes.push(nxtNode);


		UpdateNode(nxtNode, curNode, gCost, hCost, mCost);

		// restore ordering in case nxtNode was already open
		// (changing the f-cost of an OPEN node messes up the
		// queue's internal consistency; a pushed node remains
		// OPEN until it gets popped)
		openNodes.resort(nxtNode);
		openNodes.check_heap_property(0);
	}
}

void QTPFS::PathSearch::Finalize(IPath* path) {
	TracePath(path);

	#ifdef QTPFS_SMOOTH_PATHS
	SmoothPath(path);
	#endif

	path->SetBoundingBox();

	// path remains in live-cache until DeletePath is called
	pathCache->AddLivePath(path);
}

void QTPFS::PathSearch::TracePath(IPath* path) {
	std::list<float3> points;
//	std::list<float3>::const_iterator pointsIt;

	if (srcNode != tgtNode) {
		INode* tmpNode = tgtNode;
		INode* prvNode = tmpNode->GetPrevNode();

		float3 prvPoint = tgtPoint;

		while ((prvNode != NULL) && (tmpNode != srcNode)) {
			const float3& tmpPoint = tmpNode->GetNeighborEdgeTransitionPoint(prvNode, prvPoint);

			assert(!math::isinf(tmpPoint.x) && !math::isinf(tmpPoint.z));
			assert(!math::isnan(tmpPoint.x) && !math::isnan(tmpPoint.z));
			// NOTE:
			//   waypoints should NEVER have identical coordinates
			//   one exception: tgtPoint can legitimately coincide
			//   with first transition-point, which we must ignore
			assert(tmpNode != prvNode);
			assert(tmpPoint != prvPoint || tmpNode == tgtNode);

			if (tmpPoint != prvPoint) {
				points.push_front(tmpPoint);
			}

			#ifndef QTPFS_SMOOTH_PATHS
			// make sure the back-pointers can never become dangling
			// (if smoothing IS enabled, we delay this until we reach
			// SmoothPath() because we still need them there)
			tmpNode->SetPrevNode(NULL);
			#endif

			prvPoint = tmpPoint;
			tmpNode = prvNode;
			prvNode = tmpNode->GetPrevNode();
		}
	}

	// if source equals target, we need only two points
	if (!points.empty()) {
		path->AllocPoints(points.size() + 2);
	} else {
		assert(path->NumPoints() == 2);
	}

	// set waypoints with indices [1, N - 2] (if any)
	while (!points.empty()) {
		path->SetPoint((path->NumPoints() - points.size()) - 1, points.front());
		points.pop_front();
	}

	// set the first (0) and last (N - 1) waypoint
	path->SetSourcePoint(srcPoint);
	path->SetTargetPoint(tgtPoint);
}

void QTPFS::PathSearch::SmoothPath(IPath* path) {
	if (path->NumPoints() == 2)
		return;

	INode* n0 = tgtNode;
	INode* n1 = tgtNode;

	assert(srcNode->GetPrevNode() == NULL);

	// smooth in reverse order (target to source)
	unsigned int ni = path->NumPoints();

	while (n1 != srcNode) {
		n0 = n1;
		n1 = n0->GetPrevNode();
		n0->SetPrevNode(NULL);
		ni -= 1;

		assert(n1->GetNeighborRelation(n0) != 0);
		assert(n0->GetNeighborRelation(n1) != 0);
		assert(ni < path->NumPoints());

		const unsigned int ngbRel = n0->GetNeighborRelation(n1);
		const float3& p0 = path->GetPoint(ni    );
		      float3  p1 = path->GetPoint(ni - 1);
		const float3& p2 = path->GetPoint(ni - 2);

		// check if we can reduce the angle between segments
		// p0-p1 and p1-p2 (ideally to zero degrees, making
		// p0-p2 a straight line) without causing either of
		// the segments to cross into other nodes
		//
		// p1 always lies on the node to the right and/or to
		// the bottom of the shared edge between p0 and p2,
		// and we move it along the edge-dimension (x or z)
		// between [xmin, xmax] or [zmin, zmax]
		const float3 p1p0 = (p1 - p0).SafeNormalize();
		const float3 p2p1 = (p2 - p1).SafeNormalize();
		const float3 p2p0 = (p2 - p0).SafeNormalize();
		const float   dot = p1p0.dot(p2p1);

		// if segments are already nearly parallel, skip
		if (dot >= 0.995f)
			continue;

		// figure out if p1 is on a horizontal or a vertical edge
		// (if both of these are true, it is in fact in a corner)
		const bool hEdge = (((ngbRel & REL_NGB_EDGE_T) != 0) || ((ngbRel & REL_NGB_EDGE_B) != 0));
		const bool vEdge = (((ngbRel & REL_NGB_EDGE_L) != 0) || ((ngbRel & REL_NGB_EDGE_R) != 0));

		assert(hEdge || vEdge);

		// establish the x- and z-range within which p1 can be moved
		const unsigned int xmin = std::max(n1->xmin(), n0->xmin());
		const unsigned int zmin = std::max(n1->zmin(), n0->zmin());
		const unsigned int xmax = std::min(n1->xmax(), n0->xmax());
		const unsigned int zmax = std::min(n1->zmax(), n0->zmax());

		{
			// calculate intersection point between ray (p2 - p0) and edge
			// if pi lies between bounds, use that and move to next triplet
			//
			// cases:
			//     A) p0-p1-p2 (p2p0.xz >= 0 -- p0 in n0, p2 in n1)
			//     B) p2-p1-p0 (p2p0.xz <= 0 -- p2 in n1, p0 in n0)
			float3 pi = ZeroVector;

			// x- and z-distances to edge between n0 and n1
			const float dfx = (p2p0.x > 0.0f)?
				((n0->xmax() * SQUARE_SIZE) - p0.x): // A(x)
				((n0->xmin() * SQUARE_SIZE) - p0.x); // B(x)
			const float dfz = (p2p0.z > 0.0f)?
				((n0->zmax() * SQUARE_SIZE) - p0.z): // A(z)
				((n0->zmin() * SQUARE_SIZE) - p0.z); // B(z)

			const float dx = (math::fabs(p2p0.x) > 0.001f)? p2p0.x: 0.001f;
			const float dz = (math::fabs(p2p0.z) > 0.001f)? p2p0.z: 0.001f;
			const float tx = dfx / dx;
			const float tz = dfz / dz;

			bool ok = true;

			if (hEdge) {
				pi.x = p0.x + p2p0.x * tz;
				pi.z = p1.z;
			}
			if (vEdge) {
				pi.x = p1.x;
				pi.z = p0.z + p2p0.z * tx;
			}

			ok = ok && (pi.x >= (xmin * SQUARE_SIZE) && pi.x <= (xmax * SQUARE_SIZE));
			ok = ok && (pi.z >= (zmin * SQUARE_SIZE) && pi.z <= (zmax * SQUARE_SIZE));

			if (ok) {
				assert(!math::isinf(pi.x) && !math::isinf(pi.z));
				assert(!math::isnan(pi.x) && !math::isnan(pi.z));
				path->SetPoint(ni - 1, pi);
				continue;
			}
		}

		if (hEdge != vEdge) {
			// get the edge end-points
			float3 e0 = p1;
			float3 e1 = p1;

			if (hEdge) {
				e0.x = xmin * SQUARE_SIZE;
				e1.x = xmax * SQUARE_SIZE;
			}
			if (vEdge) {
				e0.z = zmin * SQUARE_SIZE;
				e1.z = zmax * SQUARE_SIZE;
			}

			// figure out what the angle between p0-p1 and p1-p2
			// would be after substituting the edge-ends for p1
			// (we want dot-products as close to 1 as possible)
			//
			// p0-e0-p2
			const float3 e0p0 = (e0 - p0).SafeNormalize();
			const float3 p2e0 = (p2 - e0).SafeNormalize();
			const float  dot0 = e0p0.dot(p2e0);
			// p0-e1-p2
			const float3 e1p0 = (e1 - p0).SafeNormalize();
			const float3 p2e1 = (p2 - e1).SafeNormalize();
			const float  dot1 = e1p0.dot(p2e1);

			// if neither end-point is an improvement, skip
			if (dot > std::max(dot0, dot1))
				continue;

			if (dot0 > std::max(dot1, dot)) { p1 = e0; }
			if (dot1 > std::max(dot0, dot)) { p1 = e1; }

			assert(!math::isinf(p1.x) && !math::isinf(p1.z));
			assert(!math::isnan(p1.x) && !math::isnan(p1.z));
			path->SetPoint(ni - 1, p1);
		}
	}
}



bool QTPFS::PathSearch::SharedFinalize(const IPath* srcPath, IPath* dstPath) {
	assert(dstPath->GetID() != 0);
	assert(dstPath->GetID() != srcPath->GetID());
	assert(dstPath->NumPoints() == 2);

	const float3& p0 = srcPath->GetTargetPoint();
	const float3& p1 = dstPath->GetTargetPoint();

	if (p0.SqDistance(p1) < (SQUARE_SIZE * SQUARE_SIZE)) {
		// copy <srcPath> to <dstPath>
		dstPath->CopyPoints(*srcPath);
		dstPath->SetSourcePoint(srcPoint);
		dstPath->SetTargetPoint(tgtPoint);
		dstPath->SetBoundingBox();

		pathCache->AddLivePath(dstPath);
		return true;
	}

	return false;
}

const boost::uint64_t QTPFS::PathSearch::GetHash(unsigned int N, unsigned int k) const {
	return (srcNode->GetNodeNumber() + (tgtNode->GetNodeNumber() * N) + (k * N * N));
}

