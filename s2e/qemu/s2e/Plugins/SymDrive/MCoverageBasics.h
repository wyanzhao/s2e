#ifndef MCOVERAGE_BASICS_H
#define MCOVERAGE_BASICS_H

namespace s2etools
{
    struct BasicBlock
    {
        uint64_t timeStamp;
        uint64_t start;
        uint64_t end;
        bool operator()(const BasicBlock&b1, const BasicBlock &b2) const {
            return b1.end < b2.start;
        }

        BasicBlock(uint64_t s, uint64_t e) {
            start = s;
            end = e;
            timeStamp = 0;
        }

        BasicBlock() {
            timeStamp = 0;
            start = end = 0;
        }

        struct SortByTime {

            bool operator()(const BasicBlock&b1, const BasicBlock &b2) const {
                if (b1.timeStamp < b2.timeStamp) {
                    return true;
                }
                return b1.start < b2.start;
            }
        };
    };

    // Either a BB or TB depending on the context
    struct Block
    {
        uint64_t timeStamp;
        uint64_t start;
        uint64_t end;

        bool operator()(const Block&b1, const Block &b2) const {
            return b1.start < b2.start;
        }

        Block() {
            timeStamp = start = end = 0;
        }

        Block(uint64_t ts, uint64_t s, uint64_t e) {
            timeStamp = ts;
            start = s;
            end = e;
        }
    };

    typedef std::set<BasicBlock, BasicBlock> BasicBlocks;
    typedef std::set<Block, Block> Blocks;
    typedef std::set<BasicBlock, BasicBlock::SortByTime> BlocksByTime;
    typedef std::map<std::string, BasicBlocks> Functions;
    typedef std::map<uint64_t, std::string> BBToFunction;
}

#endif
