#ifndef BASIC_BLOCKS_H
#define BASIC_BLOCKS_H

#include "Tree.h"

class BasicBlocks
{
public:
	BasicBlocks(tree::StmList stms);

	std::list<tree::StmList> blocks;
	Label *done;
private:
	tree::StmList block;
	void doStms(tree::StmList l);
	void mkBlocks(tree::StmList l);
};

#endif //BASIC_BLOCKS_H
