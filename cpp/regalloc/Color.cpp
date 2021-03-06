//#define ENABLE_FUNCLOG
//#define LOG_MASK (ERROR_ON | WARN_ON | INFO_ON | DEBUG_ON | VERBOSE_ON)
#include <boost/foreach.hpp>
#include "Color.h"
#include "InterferenceGraph.h"
#include "tiger.h"
#include <algorithm>

#define ENABLE_COALESCE
using namespace graph;

namespace regalloc {

Color::Color(const InterferenceGraph &ig, const TempList &regs)
	: tempSize(ig.size())
	, igraph(ig)
	, regs(regs)
	, simplifyWorkList(tempSize)
	, freezeWorkList(tempSize)
	, spillWorkList(tempSize)
	, spilledNodes(tempSize)
	, coalescedNodes(tempSize)
	, coloredNodes(tempSize)
	, coalescedMoves()
	, constrainedMoves()
	, frozenMoves()
	, workListMoves()
	, activeMoves()
	, precolored(tempSize)
	, spillTemps()
	, numMaxRegs(regs.size())
	, numWorkRegs(regs.size())
{
}

bool
Color::coloring()
{
	build();
	makeWorkList();
#if (LOG_MASK & VERBOSE_ON)
	show();
#endif
	int i = 0;
	do {
		VDBG("===== coloring iteration %d =====", i);
		++i;

		if (!simplifyWorkList.none()) {
			simplify();
		}
#ifdef ENABLE_COALESCE
		else if (!workListMoves.empty()) {
			coalesce();
		}
		else if (!freezeWorkList.none()) {
			freeze();
		}
#endif
		else if (!spillWorkList.none()) {
			selectSpill();
		}
#if (LOG_MASK & VERBOSE_ON)
		show();
#endif
	} while (!simplifyWorkList.none() ||
#ifdef ENABLE_COALESCE
			 !workListMoves.empty() ||
			 !freezeWorkList.none() ||
#endif
			 !spillWorkList.none()
			 );

	assignColors();

	if (!spilledNodes.none()) {
		//TODO:select node by spill cost
		int count = 0;
		for (unsigned int i = 0; i < spilledNodes.size(); ++i) {
			if (spilledNodes.get(i)) ++count;
		}
		int numNeedSpills = count;//spilledNodes.count();
		for (int nid = 0; (nid < tempSize) && (numNeedSpills); ++nid) {
			graph::Node *n;
			if (!precolored.get(nid)) {
				n = igraph.nid2node(nid);
				spillTemps.push_back(igraph.node2temp(n));
				//break;
				numNeedSpills--;
			}
		}
		assert(!spillTemps.empty());
		return false;
	}
	return true;
}

void 
Color::build()
{
	const NodeList &nodes = igraph.getNodes();
	BOOST_FOREACH(Node *n, nodes) {
		int nid = igraph.node2nid(n);
		degreeMap[nid] = n->degree();
		aliasMap[nid] = nid;
		color[nid] = -1;
	}
	//precolor
	int regnum = 0;
	BOOST_FOREACH(Temp *r, regs) {
		Node *regnode = igraph.temp2node(r);
		if (regnode) {
			int regnode_id = igraph.node2nid(regnode);
			//DBG("%s(r[%d]:%d) is precolored", r->toString().c_str(), regnum, regnode_id);
			precolored.set(regnode_id);
			color[regnode_id] = regnum;
		}
		++regnum;
	}
	/*
	for (unsigned int i = 0; i < precolored.size(); ++i) {
		if (precolored.get(i)) {
			for (unsigned int j = 0; j < precolored.size(); ++j) {
				if (precolored.get(j) && (i != j)) {
					Node *n1 = igraph.nid2node(i);
					Node *n2 = igraph.nid2node(j);
					igraph.addEdge(n1, n2);
				}
			}
		}
	}
	*/
	const InterferenceGraph::Moves &moves = igraph.getMoves();
	InterferenceGraph::Moves::const_iterator it = moves.begin();
	while (it != moves.end()) {
		Node *x = it->first;
		Node *y = it->second;
		int nidx = igraph.node2nid(x);
		int nidy = igraph.node2nid(y);
		if (nidx != nidy) {
			NidPair mv = std::make_pair(nidx, nidy);
			VDBG("workListMoves.push %d:%d", nidx, nidy);
			workListMoves.push_back(mv);
			moveList[nidx].push_back(mv);
			moveList[nidy].push_back(mv);
		}
		++it;
	}
#if (LOG_MASK & VERBOSE_ON)
	//igraph.show();
#endif
}

void 
Color::makeWorkList()
{
	int i = 0;
	const NodeList &nodes = igraph.getNodes();
	BOOST_FOREACH(Node *n, nodes) {
		int nid = igraph.node2nid(n);
		if (precolored.get(nid)) {
		}
		else if (numMaxRegs <= n->degree()) {
			assert(!precolored.get(i));
			spillWorkList.set(i);
		}
#ifdef ENABLE_COALESCE
		else if (isMoveRelated(i)) {
			assert(!precolored.get(i));
			freezeWorkList.set(i);
		}
#endif
		else {
			assert(!precolored.get(i));
			simplifyWorkList.set(i);
		} 
		++i;
	} 
}

void 
Color::simplify()
{
	assert(!simplifyWorkList.none());

	int nid = simplifyWorkList.right();
	assert(!precolored.get(nid));
	simplifyWorkList.reset(nid);

	Node *n = igraph.nid2node(nid);
	VDBG("selectStack.push %d(%s)", nid, (const char*)(*n));
	selectStack.push_back(n);

	Bitmap adj = adjacent(nid);
	for (unsigned int i = 0; i < adj.size(); ++i) {
		if (adj.get(i)) {
			decrementDegree(i);
		}
	}
}

void
Color::decrementDegree(int nid)
{
	int degree = degreeMap[nid];
	degreeMap[nid] = degree - 1;
	if (degree == numMaxRegs) {
		Bitmap adj = adjacent(nid);
		adj.set(nid);
		enableMoves(adj);
		spillWorkList.reset(nid);
		if (precolored.get(nid)) {
			DBG("??");
			return;
		}
		if (isMoveRelated(nid)) {
			freezeWorkList.set(nid);
		} else {
			simplifyWorkList.set(nid);
		}
	}
}

void
Color::enableMoves(const Bitmap &nodes)
{
	for (unsigned int nid = 0; nid < nodes.size(); ++nid) {
		if (!nodes.get(nid)) {
			continue;
		}
		enableMoves(nid);
	}
}

void
Color::enableMoves(int nid)
{
	FUNCLOG;
	Moves moves = nodeMoves(nid);
	BOOST_FOREACH(NidPair mv, moves) {
		Moves::iterator it = std::find(activeMoves.begin(), activeMoves.end(), mv);
		if (it != activeMoves.end()) {
			activeMoves.erase(it);
			VDBG("workListMoves.push %d:%d", it->first, it->second);
			workListMoves.push_back(*it);
		}
	}
}

void 
Color::addWorkList(int nid)
{
	if (!precolored.get(nid) && !isMoveRelated(nid) && degreeMap[nid] < numMaxRegs) {
		freezeWorkList.reset(nid);
		simplifyWorkList.set(nid);
	}
}

bool 
Color::isOK(int nid1, int nid2)
{
	Node *n1 = igraph.nid2node(nid1);
	Bitmap adj = nodes2bitmap(n1->adj());
	return degreeMap[nid1] < numMaxRegs || 
		precolored.get(nid1) ||
		adj.get(nid2);
}

bool 
Color::isConservative(const Bitmap &nodes)
{
	int k = 0;
	for (unsigned int nid = 0; nid < nodes.size(); ++nid) {
		if (!nodes.get(nid)) {
			continue;
		}
		if (degreeMap[nid] >= numMaxRegs) {
			++k;
		}
	}
	return (k < numMaxRegs);
}

void
Color::coalesce() 
{
	assert(!workListMoves.empty());
	
	VDBG("workListMoves.size() = %u", workListMoves.size());
	NidPair mv = workListMoves.back();
	workListMoves.pop_back();
	//workListMoves.erase(workListMoves.end()-1);

	int x = getAlias(mv.first);
	int y = getAlias(mv.second);
	int u, v;
	if (precolored.get(y)) {
		u = y;
		v = x;
	} else {
		u = x;
		v = y;
	}
	if (u == v) {
		coalescedMoves.push_back(mv);
		addWorkList(u);
	} else if (precolored.get(v) || adj(u, v)) {
		constrainedMoves.push_back(mv);
		addWorkList(u);
		addWorkList(v);
	} else {
		Bitmap adju = adjacent(u);
		Bitmap adjv = adjacent(v);

		bool ok = true;
		for (unsigned int a = 0; a < adjv.size(); ++a) {
			if (!isOK(a, u)) {
				ok = false;
				break;
			}
		}
		if ((precolored.get(u) && ok) || 
			(!precolored.get(u) && isConservative(adju | adjv))) {
			coalescedMoves.push_back(mv);
			//TODO: 合併済みノードの合併
			combine(u, v);
			addWorkList(u);
		} else {
			activeMoves.push_back(mv);
		}
	}
}

void 
Color::combine(int nid1, int nid2)
{
	VDBG("combine %d(%s) => %d(%s)", 
		nid2, igraph.nid2node(nid2)->toString().c_str(),
		nid1, igraph.nid2node(nid1)->toString().c_str());

	if (freezeWorkList.get(nid2)) {
		freezeWorkList.reset(nid2);
	} else {
		spillWorkList.reset(nid2);
	}
	coalescedNodes.set(nid2);
	aliasMap[nid2] = nid1;
	//combine move list
	Moves &moves1 = moveList[nid1];
	Moves &moves2 = moveList[nid2];
	Moves::iterator i = moves2.begin();
	while (i != moves2.end()) {
		int other = i->first != nid2 ? i->first:i->second;
		if (other != nid1) {
			moves1.push_back(std::make_pair(nid1, other));
		}
		++i;
	}

	enableMoves(nid2);
	Bitmap adj2 = adjacent(nid2);
	VDBG("combined nid2 adj %s", adj2.toString().c_str());
	for (unsigned int a = 0; a < adj2.size(); ++a) {
		if (!adj2.get(a)) {
			continue;
		}
		Node *n1 = igraph.nid2node(a);
		Node *n2 = igraph.nid2node(nid1);
		igraph.addEdge(n1, n2);
		decrementDegree(a);
	}
	
	if (degreeMap[nid1] >= numMaxRegs && freezeWorkList.get(nid1)) {
		assert(!precolored.get(nid1));
		freezeWorkList.reset(nid1);
		spillWorkList.set(nid1);
	}
	//igraph.show();
}

int
Color::getAlias(int nid)
{
	if (coalescedNodes.get(nid)) {
		return getAlias(aliasMap[nid]);
	} else {
		return nid;
	}
}

void
Color::freeze()
{
	int nid = freezeWorkList.right();
	VDBG("freeze %d(%s)", nid, (const char*)(*igraph.nid2node(nid)));
	assert(!precolored.get(nid));
	freezeWorkList.reset(nid);
	simplifyWorkList.set(nid);
	freezeMoves(nid);
}


void
Color::freezeMoves(int nid)
{
	Moves moves = nodeMoves(nid);
	Moves::iterator it = moves.begin();
	while (it != moves.end()) {
		int x = it->first;
		int y = it->second;
		int v;
		if (getAlias(y) == getAlias(nid)) {
			v = getAlias(x);
		} else {
			v = getAlias(y);
		}

		NidPair mv = std::make_pair(x,y);
		Moves::iterator it2 = std::find(activeMoves.begin(), activeMoves.end(), mv);
		if (it2 != activeMoves.end()) {
			activeMoves.erase(it2);
		}

		frozenMoves.push_back(mv);
		if (nodeMoves(v).empty() && degreeMap[v] < numMaxRegs && !precolored.get(v)) {
			freezeWorkList.reset(v);
			simplifyWorkList.set(v);
		}
		++it;
	}

}

void
Color::selectSpill()
{
	FUNCLOG;
	//FIXME:
	int nid = spillWorkList.right();
	assert(!precolored.get(nid));
	spillWorkList.reset(nid);
	simplifyWorkList.set(nid);
	freezeMoves(nid);
}

void
Color::assignColors() 
{ 
	FUNCLOG;
	while (!selectStack.empty()) {
		Node *n = selectStack.pop_back();
		Bitmap okColors(numMaxRegs);
		for (int i = 0; i < numMaxRegs; ++i) {
			okColors.set(i);
		}
		VDBG("assign color for %s", n->toString().c_str());
		int nid = igraph.node2nid(n);
#if 0
		Bitmap adj = adjacent(nid);
		for (unsigned int i = 0; i < adj.size(); ++i) {
			if (!adj.get(i)) {
				continue;
			}
			int reala = getAlias(i);
			//干渉辺がすでに色付けされている?
			if (coloredNodes.get(reala) || precolored.get(reala)) {
				okColors.reset(color[reala]);
			}
		}
#else
		NodeList adj = n->adj();
		BOOST_FOREACH(Node *a, adj) {
			int reala = getAlias(igraph.node2nid(a));
			//干渉辺がすでに色付けされている?
			if (coloredNodes.get(reala) || precolored.get(reala)) {
				okColors.reset(color[reala]);
			}
		}
#endif

		VDBG("ok colors %s", okColors.toString().c_str());
		
		if (!okColors.none() && okColors.right() < numWorkRegs) {
			coloredNodes.set(nid);
			if (!precolored.get(nid)) {
				color[nid] = okColors.right();
			}
		} else {
			assert(!precolored.get(nid));
			spilledNodes.set(nid);
		}
	}
	for (unsigned int nid = 0; nid < coalescedNodes.size(); ++nid) {
		if (!coalescedNodes.get(nid)) {
			continue;
		}
		color[nid] = color[getAlias(nid)];
	}
	
	ColorMap::iterator it = color.begin();
	while (it != color.end()) {
		int nid = it->first;
		int color = it->second;
		VDBG("%d color is %d", nid, color);
		++it;
	}
	
}

Bitmap
Color::adjacent(int nid)
{
	Node *n =  igraph.nid2node(nid);
	Bitmap adj = nodes2bitmap(n->adj());
	//Bitmap selects = nodes2bitmap(selectStack);
	//Bitmap tmp = selects/* | coalescedNodes FIXME*/;
	//adj -=  tmp;
	return adj;
}

Color::Moves
Color::nodeMoves(int nid)
{
	Moves &moves = moveList[nid];
#if (LOG_MASK & VERBOSE_ON)
	Moves::iterator i = moves.begin();
	while (i != moves.end()) {
		DBG("moveList[%d] %d %d", nid, i->first, i->second);
		++i;
	}
#endif
	if (!moves.empty()) {
		//TODO:  moves & (activeMoves | workListMoves);
		Moves tmp;
		tmp = activeMoves;
		std::copy(workListMoves.begin(), workListMoves.end(), std::back_inserter(tmp));
		Moves tmp2;
		Moves::iterator it = moves.begin();
		while (it != moves.end()) {
			if (std::find(tmp.begin(), tmp.end(), *it) != tmp.end()) {
				tmp2.push_back(*it);
			}
			++it;
		}
		return tmp2;
	} else {
		return Moves();
	}
}

bool
Color::isMoveRelated(int nid)
{
	Moves moves = nodeMoves(nid);
	return !moves.empty();
}

Temp *
Color::tempMap(Temp *temp)
{
	Node *n = igraph.temp2node(temp);
	if (!n) {
		return NULL;
	}
	int nid = igraph.node2nid(n);
	int regnum = color[nid];
	if (regnum == -1) {
		//ERROR("%s is not allocated to a register", temp->toString().c_str());
		return NULL;
	}
	usedRegs.push_back(regs[regnum]);
	return regs[regnum];
}


Bitmap 
Color::nodes2bitmap(const graph::NodeList &nodes)
{
	Bitmap bm(tempSize);
	BOOST_FOREACH(Node *n, nodes) {
		bm.set(igraph.node2nid(n));
	}
	return bm;
}

bool
Color::adj(int nid1, int nid2)
{
	Node *n1 = igraph.nid2node(nid1);
	Node *n2 = igraph.nid2node(nid2);
	return n1->adj(n2);
}

void
Color::show()
{
	VDBG("===== show begin =====");
	int maxid = igraph.getNodes().size();
	for (int i = 0; i < maxid; ++i) {
		int ai = getAlias(i);
		Node *n = igraph.nid2node(i);
		Node *an = igraph.nid2node(ai);
		VDBG("alias: %d(%s) => %d(%s)", 
			i, n->toString().c_str(),
			ai, an->toString().c_str());
	}
	Bitmap stk = nodes2bitmap(selectStack);
	VDBG("simplifyWorkList: %s", (const char*)simplifyWorkList);
	VDBG("freezeWorkList  : %s", (const char*)freezeWorkList);
	VDBG("spillWorkList   : %s", (const char*)spillWorkList);
	VDBG("spilledNodes    : %s", (const char*)spilledNodes);
	VDBG("coalescedNodes  : %s", (const char*)coalescedNodes);
	VDBG("selectStack     : %s", (const char*)stk);
	VDBG("===== show end =====\n");
}

TempList
Color::getTempForSpill()
{
	return spillTemps;
}

void 
Color::setNumWorkRegs(int n)
{
	numWorkRegs = n;
}

TempList
Color::getUsedRegs()
{
	std::sort(usedRegs.begin(), usedRegs.end(), LessTemp());
	usedRegs.erase(std::unique(usedRegs.begin(), usedRegs.end()), usedRegs.end());
	Temp::printTempList(usedRegs);
	return usedRegs;
}

}//namespace regalloc
