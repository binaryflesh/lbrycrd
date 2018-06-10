#include "claimtrie.h"
#include "coins.h"
#include "hash.h"

#include <iostream>
#include <algorithm>

#define HASH_BLOCK 'h'
#define CURRENT_HEIGHT 't'

#include "claimtriedb.cpp" /// avoid linker problems

const uint256 one = uint256S("0000000000000000000000000000000000000000000000000000000000000001");

std::vector<unsigned char> heightToVch(int n)
{
    std::vector<unsigned char> vchHeight;
    vchHeight.resize(8);
    vchHeight[0] = 0;
    vchHeight[1] = 0;
    vchHeight[2] = 0;
    vchHeight[3] = 0;
    vchHeight[4] = n >> 24;
    vchHeight[5] = n >> 16;
    vchHeight[6] = n >> 8;
    vchHeight[7] = n;
    return vchHeight;
}

uint256 getValueHash(COutPoint outPoint, int nHeightOfLastTakeover)
{
    CHash256 txHasher;
    txHasher.Write(outPoint.hash.begin(), outPoint.hash.size());
    std::vector<unsigned char> vchtxHash(txHasher.OUTPUT_SIZE);
    txHasher.Finalize(&(vchtxHash[0]));

    CHash256 nOutHasher;
    std::stringstream ss;
    ss << outPoint.n;
    std::string snOut = ss.str();
    nOutHasher.Write((unsigned char*) snOut.data(), snOut.size());
    std::vector<unsigned char> vchnOutHash(nOutHasher.OUTPUT_SIZE);
    nOutHasher.Finalize(&(vchnOutHash[0]));

    CHash256 takeoverHasher;
    std::vector<unsigned char> vchTakeoverHeightToHash = heightToVch(nHeightOfLastTakeover);
    takeoverHasher.Write(vchTakeoverHeightToHash.data(), vchTakeoverHeightToHash.size());
    std::vector<unsigned char> vchTakeoverHash(takeoverHasher.OUTPUT_SIZE);
    takeoverHasher.Finalize(&(vchTakeoverHash[0]));

    CHash256 hasher;
    hasher.Write(vchtxHash.data(), vchtxHash.size());
    hasher.Write(vchnOutHash.data(), vchnOutHash.size());
    hasher.Write(vchTakeoverHash.data(), vchTakeoverHash.size());
    std::vector<unsigned char> vchHash(hasher.OUTPUT_SIZE);
    hasher.Finalize(&(vchHash[0]));

    uint256 valueHash(vchHash);
    return valueHash;
}

bool CClaimTrieNode::insertClaim(CClaimValue claim)
{
    LogPrintf("%s: Inserting %s:%d (amount: %d)  into the claim trie\n", __func__, claim.outPoint.hash.ToString(), claim.outPoint.n, claim.nAmount);
    claims.push_back(claim);
    return true;
}

bool CClaimTrieNode::removeClaim(const COutPoint& outPoint, CClaimValue& claim)
{
    LogPrintf("%s: Removing txid: %s, nOut: %d from the claim trie\n", __func__, outPoint.hash.ToString(), outPoint.n);

    std::vector<CClaimValue>::iterator itClaims;
    for (itClaims = claims.begin(); itClaims != claims.end(); ++itClaims)
    {
        if (itClaims->outPoint == outPoint)
        {
            std::swap(claim, *itClaims);
            break;
        }
    }
    if (itClaims != claims.end())
    {
        claims.erase(itClaims);
    }
    else
    {
        LogPrintf("CClaimTrieNode::%s() : asked to remove a claim that doesn't exist\n", __func__);
        LogPrintf("CClaimTrieNode::%s() : claims that do exist:\n", __func__);
        for (unsigned int i = 0; i < claims.size(); i++)
        {
            LogPrintf("\ttxhash: %s, nOut: %d:\n", claims[i].outPoint.hash.ToString(), claims[i].outPoint.n);
        }
        return false;
    }
    return true;
}

bool CClaimTrieNode::getBestClaim(CClaimValue& claim) const
{
    if (claims.empty())
    {
        return false;
    }
    else
    {
        claim = claims.front();
        return true;
    }
}

bool CClaimTrieNode::haveClaim(const COutPoint& outPoint) const
{
    for (std::vector<CClaimValue>::const_iterator itclaim = claims.begin(); itclaim != claims.end(); ++itclaim)
    {
        if (itclaim->outPoint == outPoint)
            return true;
    }
    return false;
}

void CClaimTrieNode::reorderClaims(supportMapEntryType& supports)
{
    std::vector<CClaimValue>::iterator itclaim;

    for (itclaim = claims.begin(); itclaim != claims.end(); ++itclaim)
    {
        itclaim->nEffectiveAmount = itclaim->nAmount;
    }

    for (supportMapEntryType::iterator itsupport = supports.begin(); itsupport != supports.end(); ++itsupport)
    {
        for (itclaim = claims.begin(); itclaim != claims.end(); ++itclaim)
        {
            if (itsupport->supportedClaimId == itclaim->claimId)
            {
                itclaim->nEffectiveAmount += itsupport->nAmount;
                break;
            }
        }
    }

    std::make_heap(claims.begin(), claims.end());
}

bool CClaimTrieNode::empty() const
{
    return children.empty() && claims.empty();
}

CClaimTrie::CClaimTrie(bool fMemory, bool fWipe, int nProportionalDelayFactor)
                    : nCurrentHeight(0)
                    , nExpirationTime(Params().GetConsensus().nOriginalClaimExpirationTime)
                    , nProportionalDelayFactor(nProportionalDelayFactor)
                    , db(fMemory, fWipe)
                    , root(one)
{
}

CClaimTrie::~CClaimTrie()
{
}

uint256 CClaimTrie::getMerkleHash()
{
    return root.hash;
}

bool CClaimTrie::empty() const
{
    return root.empty();
}

bool CClaimTrie::queueEmpty() const
{
    return db.keyTypeEmpty<int, claimQueueRowType>();
}

bool CClaimTrie::expirationQueueEmpty() const
{
    return db.keyTypeEmpty<int, expirationQueueRowType>();
}

bool CClaimTrie::supportEmpty() const
{
    return db.keyTypeEmpty<std::string, supportMapEntryType>();
}

bool CClaimTrie::supportQueueEmpty() const
{
    return db.keyTypeEmpty<int, supportQueueRowType>();
}

void CClaimTrie::setExpirationTime(int t)
{
    nExpirationTime = t;
    LogPrintf("%s: Expiration time is now %d\n", __func__, nExpirationTime);
}

void CClaimTrie::clear()
{
    clear(&root);
}

void CClaimTrie::clear(CClaimTrieNode* current)
{
    for (nodeMapType::const_iterator itchildren = current->children.begin(); itchildren != current->children.end(); ++itchildren)
    {
        clear(itchildren->second);
        delete itchildren->second;
    }
}

bool CClaimTrie::haveClaim(const std::string& name, const COutPoint& outPoint) const
{
    const CClaimTrieNode* current = &root;
    for (std::string::const_iterator itname = name.begin(); itname != name.end(); ++itname)
    {
        nodeMapType::const_iterator itchildren = current->children.find(*itname);
        if (itchildren == current->children.end())
            return false;
        current = itchildren->second;
    }
    return current->haveClaim(outPoint);
}

bool CClaimTrie::haveSupport(const std::string& name, const COutPoint& outPoint) const
{
    supportMapEntryType node;
    if (!db.getQueueRow(name, node))
    {
        return false;
    }
    for (supportMapEntryType::const_iterator itnode = node.begin(); itnode != node.end(); ++itnode)
    {
        if (itnode->outPoint == outPoint)
            return true;
    }
    return false;
}

bool CClaimTrie::haveClaimInQueue(const std::string& name, const COutPoint& outPoint, int& nValidAtHeight) const
{
    queueNameRowType nameRow;
    if (!db.getQueueRow(name, nameRow))
    {
        return false;
    }
    queueNameRowType::const_iterator itNameRow;
    for (itNameRow = nameRow.begin(); itNameRow != nameRow.end(); ++itNameRow)
    {
        if (itNameRow->outPoint == outPoint)
        {
            nValidAtHeight = itNameRow->nHeight;
            break;
        }
    }
    if (itNameRow == nameRow.end())
    {
        return false;
    }
    claimQueueRowType row;
    if (db.getQueueRow(nValidAtHeight, row))
    {
        for (claimQueueRowType::const_iterator itRow = row.begin(); itRow != row.end(); ++itRow)
        {
            if (itRow->first == name && itRow->second.outPoint == outPoint)
            {
                if (itRow->second.nValidAtHeight != nValidAtHeight)
                {
                    LogPrintf("%s: An inconsistency was found in the claim queue. Please report this to the developers:\nDifferent nValidAtHeight between named queue and height queue\n: name: %s, txid: %s, nOut: %d, nValidAtHeight in named queue: %d, nValidAtHeight in height queue: %d current height: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, nValidAtHeight, itRow->second.nValidAtHeight, nCurrentHeight);
                }
                return true;
            }
        }
    }
    LogPrintf("%s: An inconsistency was found in the claim queue. Please report this to the developers:\nFound in named queue but not in height queue: name: %s, txid: %s, nOut: %d, nValidAtHeight: %d, current height: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, nValidAtHeight, nCurrentHeight);
    return false;
}

bool CClaimTrie::haveSupportInQueue(const std::string& name, const COutPoint& outPoint, int& nValidAtHeight) const
{
    supportQueueNameRowType nameRow;
    if (!db.getQueueRow(name, nameRow))
    {
        return false;
    }
    supportQueueNameRowType::const_iterator itNameRow;
    for (itNameRow = nameRow.begin(); itNameRow != nameRow.end(); ++itNameRow)
    {
        if (itNameRow->outPoint == outPoint)
        {
            nValidAtHeight = itNameRow->nHeight;
            break;
        }
    }
    if (itNameRow == nameRow.end())
    {
        return false;
    }
    supportQueueRowType row;
    if (db.getQueueRow(nValidAtHeight, row))
    {
        for (supportQueueRowType::const_iterator itRow = row.begin(); itRow != row.end(); ++itRow)
        {
            if (itRow->first == name && itRow->second.outPoint == outPoint)
            {
                if (itRow->second.nValidAtHeight != nValidAtHeight)
                {
                    LogPrintf("%s: An inconsistency was found in the support queue. Please report this to the developers:\nDifferent nValidAtHeight between named queue and height queue\n: name: %s, txid: %s, nOut: %d, nValidAtHeight in named queue: %d, nValidAtHeight in height queue: %d current height: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, nValidAtHeight, itRow->second.nValidAtHeight, nCurrentHeight);
                }
                return true;
            }
        }
    }
    LogPrintf("%s: An inconsistency was found in the claim queue. Please report this to the developers:\nFound in named queue but not in height queue: name: %s, txid: %s, nOut: %d, nValidAtHeight: %d, current height: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, nValidAtHeight, nCurrentHeight);
    return false;
}

unsigned int CClaimTrie::getTotalNamesInTrie() const
{
    if (empty())
        return 0;
    const CClaimTrieNode* current = &root;
    return getTotalNamesRecursive(current);
}

unsigned int CClaimTrie::getTotalNamesRecursive(const CClaimTrieNode* current) const
{
    unsigned int names_in_subtrie = 0;
    if (!(current->claims.empty()))
        names_in_subtrie += 1;
    for (nodeMapType::const_iterator it = current->children.begin(); it != current->children.end(); ++it)
    {
        names_in_subtrie += getTotalNamesRecursive(it->second);
    }
    return names_in_subtrie;
}

unsigned int CClaimTrie::getTotalClaimsInTrie() const
{
    if (empty())
        return 0;
    const CClaimTrieNode* current = &root;
    return getTotalClaimsRecursive(current);
}

unsigned int CClaimTrie::getTotalClaimsRecursive(const CClaimTrieNode* current) const
{
    unsigned int claims_in_subtrie = current->claims.size();
    for (nodeMapType::const_iterator it = current->children.begin(); it != current->children.end(); ++it)
    {
        claims_in_subtrie += getTotalClaimsRecursive(it->second);
    }
    return claims_in_subtrie;
}

CAmount CClaimTrie::getTotalValueOfClaimsInTrie(bool fControllingOnly) const
{
    if (empty())
        return 0;
    const CClaimTrieNode* current = &root;
    return getTotalValueOfClaimsRecursive(current, fControllingOnly);
}

CAmount CClaimTrie::getTotalValueOfClaimsRecursive(const CClaimTrieNode* current, bool fControllingOnly) const
{
    CAmount value_in_subtrie = 0;
    for (std::vector<CClaimValue>::const_iterator itclaim = current->claims.begin(); itclaim != current->claims.end(); ++itclaim)
    {
        value_in_subtrie += itclaim->nAmount;
        if (fControllingOnly)
            break;
    }
    for (nodeMapType::const_iterator itchild = current->children.begin(); itchild != current->children.end(); ++itchild)
     {
         value_in_subtrie += getTotalValueOfClaimsRecursive(itchild->second, fControllingOnly);
     }
     return value_in_subtrie;
}

bool CClaimTrie::recursiveFlattenTrie(const std::string& name, const CClaimTrieNode* current, std::vector<namedNodeType>& nodes) const
{
    namedNodeType node(name, *current);
    nodes.push_back(node);
    for (nodeMapType::const_iterator it = current->children.begin(); it != current->children.end(); ++it)
    {
        std::stringstream ss;
        ss << name << it->first;
        if (!recursiveFlattenTrie(ss.str(), it->second, nodes))
            return false;
    }
    return true;
}

std::vector<namedNodeType> CClaimTrie::flattenTrie() const
{
    std::vector<namedNodeType> nodes;
    if (!recursiveFlattenTrie("", &root, nodes))
        LogPrintf("%s: Something went wrong flattening the trie", __func__);
    return nodes;
}

const CClaimTrieNode* CClaimTrie::getNodeForName(const std::string& name) const
{
    const CClaimTrieNode* current = &root;
    for (std::string::const_iterator itname = name.begin(); itname != name.end(); ++itname)
    {
        nodeMapType::const_iterator itchildren = current->children.find(*itname);
        if (itchildren == current->children.end())
            return NULL;
        current = itchildren->second;
    }
    return current;
}

bool CClaimTrie::getInfoForName(const std::string& name, CClaimValue& claim) const
{
    const CClaimTrieNode* current = getNodeForName(name);
    if (current)
    {
        return current->getBestClaim(claim);
    }
    return false;
}

bool CClaimTrie::getLastTakeoverForName(const std::string& name, int& lastTakeoverHeight) const
{
    const CClaimTrieNode* current = getNodeForName(name);
    if (current && !current->claims.empty())
    {
        lastTakeoverHeight = current->nHeightOfLastTakeover;
        return true;
    }
    return false;
}

claimsForNameType CClaimTrie::getClaimsForName(const std::string& name) const
{
    std::vector<CClaimValue> claims;
    std::vector<CSupportValue> supports;
    int nLastTakeoverHeight = 0;
    const CClaimTrieNode* current = getNodeForName(name);
    if (current)
    {
        if (!current->claims.empty())
        {
            nLastTakeoverHeight = current->nHeightOfLastTakeover;
        }
        for (std::vector<CClaimValue>::const_iterator itClaims = current->claims.begin(); itClaims != current->claims.end(); ++itClaims)
        {
            claims.push_back(*itClaims);
        }
    }
    supportMapEntryType supportNode;
    if (db.getQueueRow(name, supportNode))
    {
        for (std::vector<CSupportValue>::const_iterator itSupports = supportNode.begin(); itSupports != supportNode.end(); ++itSupports)
        {
            supports.push_back(*itSupports);
        }
    }
    queueNameRowType namedClaimRow;
    if (db.getQueueRow(name, namedClaimRow))
    {
        for (queueNameRowType::const_iterator itClaimsForName = namedClaimRow.begin(); itClaimsForName != namedClaimRow.end(); ++itClaimsForName)
        {
            claimQueueRowType claimRow;
            if (db.getQueueRow(itClaimsForName->nHeight, claimRow))
            {
                for (claimQueueRowType::const_iterator itClaimRow = claimRow.begin(); itClaimRow != claimRow.end(); ++itClaimRow)
                 {
                     if (itClaimRow->first == name && itClaimRow->second.outPoint == itClaimsForName->outPoint)
                     {
                         claims.push_back(itClaimRow->second);
                         break;
                     }
                 }
            }
        }
    }
    supportQueueNameRowType namedSupportRow;
    if (db.getQueueRow(name, namedSupportRow))
    {
        for (supportQueueNameRowType::const_iterator itSupportsForName = namedSupportRow.begin(); itSupportsForName != namedSupportRow.end(); ++itSupportsForName)
        {
            supportQueueRowType supportRow;
            if (db.getQueueRow(itSupportsForName->nHeight, supportRow))
            {
                for (supportQueueRowType::const_iterator itSupportRow = supportRow.begin(); itSupportRow != supportRow.end(); ++itSupportRow)
                {
                    if (itSupportRow->first == name && itSupportRow->second.outPoint == itSupportsForName->outPoint)
                    {
                        supports.push_back(itSupportRow->second);
                        break;
                    }
                }
            }
        }
    }
    claimsForNameType allClaims(claims, supports, nLastTakeoverHeight);
    return allClaims;
}

//return effective amount from claim, retuns 0 if claim is not found
CAmount CClaimTrie::getEffectiveAmountForClaim(const std::string& name, uint160 claimId) const
{
    std::vector<CSupportValue> supports;
    return getEffectiveAmountForClaimWithSupports(name, claimId, supports);
}

//return effective amount from claim and the supports used as inputs, retuns 0 if claim is not found
CAmount CClaimTrie::getEffectiveAmountForClaimWithSupports(const std::string& name, uint160 claimId,
                                                           std::vector<CSupportValue>& supports) const
{
    claimsForNameType claims = getClaimsForName(name);
    CAmount effectiveAmount = 0;
    bool claim_found = false;
    for (std::vector<CClaimValue>::iterator it=claims.claims.begin(); it!=claims.claims.end(); ++it)
    {
        if (it->claimId == claimId && it->nValidAtHeight < nCurrentHeight)
        {
            effectiveAmount += it->nAmount;
            claim_found = true;
            break;
        }
    }
    if (!claim_found)
        return effectiveAmount;

    for (std::vector<CSupportValue>::iterator it=claims.supports.begin(); it!=claims.supports.end(); ++it)
    {
        if (it->supportedClaimId == claimId && it->nValidAtHeight < nCurrentHeight)
        {
            effectiveAmount += it->nAmount;
            supports.push_back(*it);
        }
    }
    return effectiveAmount;
}

bool CClaimTrie::checkConsistency() const
{
    if (empty())
        return true;
    return recursiveCheckConsistency(&root);
}

bool CClaimTrie::recursiveCheckConsistency(const CClaimTrieNode* node) const
{
    std::vector<unsigned char> vchToHash;

    for (nodeMapType::const_iterator it = node->children.begin(); it != node->children.end(); ++it)
    {
        if (recursiveCheckConsistency(it->second))
        {
            vchToHash.push_back(it->first);
            vchToHash.insert(vchToHash.end(), it->second->hash.begin(), it->second->hash.end());
        }
        else
            return false;
    }

    CClaimValue claim;
    bool hasClaim = node->getBestClaim(claim);

    if (hasClaim)
    {
        uint256 valueHash = getValueHash(claim.outPoint, node->nHeightOfLastTakeover);
        vchToHash.insert(vchToHash.end(), valueHash.begin(), valueHash.end());
    }

    CHash256 hasher;
    std::vector<unsigned char> vchHash(hasher.OUTPUT_SIZE);
    hasher.Write(vchToHash.data(), vchToHash.size());
    hasher.Finalize(&(vchHash[0]));
    uint256 calculatedHash(vchHash);
    return calculatedHash == node->hash;
}

void CClaimTrie::addToClaimIndex(const std::string& name, const CClaimValue& claim)
{
    CClaimIndexElement element = { name, claim };
    LogPrintf("%s: ClaimIndex[%s] updated %s\n", __func__, claim.claimId.GetHex(), name);

    db.updateQueueRow(claim.claimId, element);
}

void CClaimTrie::removeFromClaimIndex(const CClaimValue& claim)
{
    CClaimIndexElement element;
    db.updateQueueRow(claim.claimId, element);
}

bool CClaimTrie::getClaimById(const uint160 claimId, std::string& name, CClaimValue& claim) const
{
    CClaimIndexElement element;
    if (db.getQueueRow(claimId, element))
    {
        name = element.name;
        claim = element.claim;
        return true;
    }
    return false;
}

void CClaimTrie::markNodeDirty(const std::string &name, CClaimTrieNode* node)
{
    std::pair<nodeCacheType::iterator, bool> ret;
    ret = dirtyNodes.insert(std::pair<std::string, CClaimTrieNode*>(name, node));
    if (ret.second == false)
        ret.first->second = node;
}

bool CClaimTrie::updateName(const std::string &name, CClaimTrieNode* updatedNode)
{
    CClaimTrieNode* current = &root;
    for (std::string::const_iterator itname = name.begin(); itname != name.end(); ++itname)
    {
        nodeMapType::iterator itchild = current->children.find(*itname);
        if (itchild == current->children.end())
        {
            if (itname + 1 == name.end())
            {
                CClaimTrieNode* newNode = new CClaimTrieNode();
                current->children[*itname] = newNode;
                current = newNode;
            }
            else
                return false;
        }
        else
        {
            current = itchild->second;
        }
    }
    assert(current != NULL);
    current->claims.swap(updatedNode->claims);
    markNodeDirty(name, current);
    for (nodeMapType::iterator itchild = current->children.begin(); itchild != current->children.end();)
    {
        nodeMapType::iterator itupdatechild = updatedNode->children.find(itchild->first);
        if (itupdatechild == updatedNode->children.end())
        {
            // This character has apparently been deleted, so delete
            // all descendents from this child.
            std::stringstream ss;
            ss << name << itchild->first;
            std::string newName = ss.str();
            if (!recursiveNullify(itchild->second, newName))
                return false;
            current->children.erase(itchild++);
        }
        else
            ++itchild;
    }
    return true;
}

bool CClaimTrie::recursiveNullify(CClaimTrieNode* node, std::string& name)
{
    assert(node != NULL);
    for (nodeMapType::iterator itchild = node->children.begin(); itchild != node->children.end(); ++itchild)
    {
        std::stringstream ss;
        ss << name << itchild->first;
        std::string newName = ss.str();
        if (!recursiveNullify(itchild->second, newName))
            return false;
    }
    node->children.clear();
    markNodeDirty(name, NULL);
    delete node;
    return true;
}

bool CClaimTrie::updateHash(const std::string& name, uint256& hash)
{
    CClaimTrieNode* current = &root;
    for (std::string::const_iterator itname = name.begin(); itname != name.end(); ++itname)
    {
        nodeMapType::iterator itchild = current->children.find(*itname);
        if (itchild == current->children.end())
            return false;
        current = itchild->second;
    }
    assert(current != NULL);
    current->hash = hash;
    markNodeDirty(name, current);
    return true;
}

bool CClaimTrie::updateTakeoverHeight(const std::string& name, int nTakeoverHeight)
{
    CClaimTrieNode* current = &root;
    for (std::string::const_iterator itname = name.begin(); itname != name.end(); ++itname)
    {
        nodeMapType::iterator itchild = current->children.find(*itname);
        if (itchild == current->children.end())
            return false;
        current = itchild->second;
    }
    assert(current != NULL);
    current->nHeightOfLastTakeover = nTakeoverHeight;
    markNodeDirty(name, current);
    return true;
}

bool CClaimTrie::WriteToDisk()
{
    for (nodeCacheType::iterator itcache = dirtyNodes.begin(); itcache != dirtyNodes.end(); ++itcache)
    {
        CClaimTrieNode *pNode = itcache->second;
        uint32_t num_claims = pNode ? pNode->claims.size() : 0;
        LogPrintf("%s: Writing %s to disk with %d claims\n", __func__, itcache->first, num_claims);
        if (pNode)
        {
            db.updateQueueRow(itcache->first, *pNode);
        }
        else
        {
            CClaimTrieNode emptyNode;
            db.updateQueueRow(itcache->first, emptyNode);
        }
    }

    dirtyNodes.clear();
    db.writeQueues();
    db.Write(HASH_BLOCK, hashBlock);
    db.Write(CURRENT_HEIGHT, nCurrentHeight);
    return db.Sync();
}

bool CClaimTrie::InsertFromDisk(const std::string& name, CClaimTrieNode* node)
{
    if (name.size() == 0)
    {
        root = *node;
        return true;
    }
    CClaimTrieNode* current = &root;
    for (std::string::const_iterator itname = name.begin(); itname + 1 != name.end(); ++itname)
    {
        nodeMapType::iterator itchild = current->children.find(*itname);
        if (itchild == current->children.end())
            return false;
        current = itchild->second;
    }
    current->children[name[name.size()-1]] = new CClaimTrieNode(*node);
    return true;
}

bool CClaimTrie::ReadFromDisk(bool check)
{
    if (!db.Read(HASH_BLOCK, hashBlock))
        LogPrintf("%s: Couldn't read the best block's hash\n", __func__);
    if (!db.Read(CURRENT_HEIGHT, nCurrentHeight))
        LogPrintf("%s: Couldn't read the current height\n", __func__);

    setExpirationTime(Params().GetConsensus().GetExpirationTime(nCurrentHeight-1));

    typedef std::map<std::string, CClaimTrieNode, nodenamecompare> trieNodeMapType;

    trieNodeMapType nodes;
    if(!db.seekByKey(nodes))
    {
        return error("%s(): error reading claim trie from disk", __func__);
    }

    for(trieNodeMapType::iterator it = nodes.begin(); it != nodes.end(); ++it)
    {
        if (!InsertFromDisk(it->first, &(it->second)))
        {
            return error("%s(): error restoring claim trie from disk", __func__);
        }
    }

    if (check)
    {
        LogPrintf("Checking Claim trie consistency...");
        if (checkConsistency())
        {
            LogPrintf("consistent\n");
            return true;
        }
        LogPrintf("inconsistent!\n");
        return false;
    }
    return true;
}

bool CClaimTrieCache::recursiveComputeMerkleHash(CClaimTrieNode* tnCurrent, std::string sPos) const
{
    if (sPos == "" && tnCurrent->empty())
    {
        cacheHashes[""] = one;
        return true;
    }
    std::vector<unsigned char> vchToHash;
    nodeCacheType::const_iterator cachedNode;


    for (nodeMapType::iterator it = tnCurrent->children.begin(); it != tnCurrent->children.end(); ++it)
    {
        std::stringstream ss;
        ss << it->first;
        std::string sNextPos = sPos + ss.str();
        if (dirtyHashes.count(sNextPos) != 0)
        {
            // the child might be in the cache, so look for it there
            cachedNode = cache.find(sNextPos);
            if (cachedNode != cache.end())
                recursiveComputeMerkleHash(cachedNode->second, sNextPos);
            else
                recursiveComputeMerkleHash(it->second, sNextPos);
        }
        vchToHash.push_back(it->first);
        hashMapType::iterator ithash = cacheHashes.find(sNextPos);
        if (ithash != cacheHashes.end())
        {
            vchToHash.insert(vchToHash.end(), ithash->second.begin(), ithash->second.end());
        }
        else
        {
            vchToHash.insert(vchToHash.end(), it->second->hash.begin(), it->second->hash.end());
        }
    }

    CClaimValue claim;
    bool hasClaim = tnCurrent->getBestClaim(claim);

    if (hasClaim)
    {
        int nHeightOfLastTakeover;
        assert(getLastTakeoverForName(sPos, nHeightOfLastTakeover));
        uint256 valueHash = getValueHash(claim.outPoint, nHeightOfLastTakeover);
        vchToHash.insert(vchToHash.end(), valueHash.begin(), valueHash.end());
    }

    CHash256 hasher;
    std::vector<unsigned char> vchHash(hasher.OUTPUT_SIZE);
    hasher.Write(vchToHash.data(), vchToHash.size());
    hasher.Finalize(&(vchHash[0]));
    cacheHashes[sPos] = uint256(vchHash);
    std::set<std::string>::iterator itDirty = dirtyHashes.find(sPos);
    if (itDirty != dirtyHashes.end())
        dirtyHashes.erase(itDirty);
    return true;
}

uint256 CClaimTrieCache::getMerkleHash() const
{
    if (empty())
    {
        return one;
    }
    if (dirty())
    {
        nodeCacheType::const_iterator cachedNode = cache.find("");
        if (cachedNode != cache.end())
            recursiveComputeMerkleHash(cachedNode->second, "");
        else
            recursiveComputeMerkleHash(&(base->root), "");
    }
    hashMapType::const_iterator ithash = cacheHashes.find("");
    if (ithash != cacheHashes.end())
        return ithash->second;
    else
        return base->root.hash;
}

bool CClaimTrieCache::empty() const
{
    return base->empty() && cache.empty();
}

CClaimTrieNode* CClaimTrieCache::addNodeToCache(const std::string& position, CClaimTrieNode* original)
{
    // create a copy of the node in the cache, if new node, create empty node
    CClaimTrieNode* cacheCopy;
    if(!original)
        cacheCopy = new CClaimTrieNode();
    else
        cacheCopy = new CClaimTrieNode(*original);
    cache[position] = cacheCopy;

    // check to see if there is the original node in block_originals,
    // if not, add it to block_originals cache
    nodeCacheType::const_iterator itOriginals = block_originals.find(position);
    if (block_originals.end() == itOriginals)
    {
        CClaimTrieNode* originalCopy;
        if(!original)
            originalCopy = new CClaimTrieNode();
        else
            originalCopy = new CClaimTrieNode(*original);
        block_originals[position] = originalCopy;
    }
    return cacheCopy;
}

bool CClaimTrieCache::getOriginalInfoForName(const std::string& name, CClaimValue& claim) const
{
    nodeCacheType::const_iterator itOriginalCache = block_originals.find(name);
    if (itOriginalCache == block_originals.end())
    {
        return base->getInfoForName(name, claim);
    }
    return itOriginalCache->second->getBestClaim(claim);
}

bool CClaimTrieCache::insertClaimIntoTrie(const std::string& name, CClaimValue claim, bool fCheckTakeover)
{
    assert(base);
    CClaimTrieNode* currentNode = &(base->root);
    nodeCacheType::iterator cachedNode;
    cachedNode = cache.find("");
    if (cachedNode != cache.end())
        currentNode = cachedNode->second;
    for (std::string::const_iterator itCur = name.begin(); itCur != name.end(); ++itCur)
    {
        std::string sCurrentSubstring(name.begin(), itCur);
        std::string sNextSubstring(name.begin(), itCur + 1);

        cachedNode = cache.find(sNextSubstring);
        if (cachedNode != cache.end())
        {
            currentNode = cachedNode->second;
            continue;
        }
        nodeMapType::iterator childNode = currentNode->children.find(*itCur);
        if (childNode != currentNode->children.end())
        {
            currentNode = childNode->second;
            continue;
        }

        // This next substring doesn't exist in the cache and the next
        // character doesn't exist in current node's children, so check
        // if the current node is in the cache, and if it's not, copy
        // it and stick it in the cache, and then create a new node as
        // its child and stick that in the cache. We have to have both
        // this node and its child in the cache so that the current
        // node's child map will contain the next letter, which will be
        // used to find the child in the cache. This is necessary in
        // order to calculate the merkle hash.
        cachedNode = cache.find(sCurrentSubstring);
        if (cachedNode != cache.end())
        {
            assert(cachedNode->second == currentNode);
        }
        else
        {
            currentNode = addNodeToCache(sCurrentSubstring, currentNode);
        }
        CClaimTrieNode* newNode = addNodeToCache(sNextSubstring, NULL);
        currentNode->children[*itCur] = newNode;
        currentNode = newNode;
    }

    cachedNode = cache.find(name);
    if (cachedNode != cache.end())
    {
        assert(cachedNode->second == currentNode);
    }
    else
    {
        currentNode = addNodeToCache(name, currentNode);
    }
    bool fChanged = false;
    if (currentNode->claims.empty())
    {
        fChanged = true;
        currentNode->insertClaim(claim);
        base->addToClaimIndex(name, claim);
    }
    else
    {
        CClaimValue currentTop = currentNode->claims.front();
        currentNode->insertClaim(claim);
        supportMapEntryType node;
        getSupportsForName(name, node);
        currentNode->reorderClaims(node);
        if (currentTop != currentNode->claims.front())
            fChanged = true;
    }
    if (fChanged)
    {
        for (std::string::const_iterator itCur = name.begin(); itCur != name.end(); ++itCur)
        {
            std::string sub(name.begin(), itCur);
            dirtyHashes.insert(sub);
        }
        dirtyHashes.insert(name);
        if (fCheckTakeover)
            namesToCheckForTakeover.insert(name);
    }
    return true;
}

bool CClaimTrieCache::removeClaimFromTrie(const std::string& name, const COutPoint& outPoint, CClaimValue& claim, bool fCheckTakeover)
{
    assert(base);
    CClaimTrieNode* currentNode = &(base->root);
    nodeCacheType::iterator cachedNode;
    cachedNode = cache.find("");
    if (cachedNode != cache.end())
        currentNode = cachedNode->second;
    assert(currentNode != NULL); // If there is no root in either the trie or the cache, how can there be any names to remove?
    for (std::string::const_iterator itCur = name.begin(); itCur != name.end(); ++itCur)
    {
        std::string sCurrentSubstring(name.begin(), itCur);
        std::string sNextSubstring(name.begin(), itCur + 1);

        cachedNode = cache.find(sNextSubstring);
        if (cachedNode != cache.end())
        {
            currentNode = cachedNode->second;
            continue;
        }
        nodeMapType::iterator childNode = currentNode->children.find(*itCur);
        if (childNode != currentNode->children.end())
        {
            currentNode = childNode->second;
            continue;
        }
        LogPrintf("%s: The name %s does not exist in the trie\n", __func__, name.c_str());
        return false;
    }

    cachedNode = cache.find(name);
    if (cachedNode != cache.end())
        assert(cachedNode->second == currentNode);
    else
    {
        currentNode = addNodeToCache(name, currentNode);
    }
    bool fChanged = false;
    assert(currentNode != NULL);
    bool success = false;

    if (currentNode->claims.empty())
    {
        LogPrintf("%s: Asked to remove claim from node without claims\n", __func__);
        return false;
    }
    CClaimValue currentTop = currentNode->claims.front();

    success = currentNode->removeClaim(outPoint, claim);
    base->removeFromClaimIndex(claim);

    if (!currentNode->claims.empty())
    {
        supportMapEntryType node;
        getSupportsForName(name, node);
        currentNode->reorderClaims(node);
        if (currentTop != currentNode->claims.front())
            fChanged = true;
    }
    else
        fChanged = true;

    if (!success)
    {
        LogPrintf("%s: Removing a claim was unsuccessful. name = %s, txhash = %s, nOut = %d", __func__, name.c_str(), outPoint.hash.GetHex(), outPoint.n);
        return false;
    }

    if (fChanged)
    {
        for (std::string::const_iterator itCur = name.begin(); itCur != name.end(); ++itCur)
        {
            std::string sub(name.begin(), itCur);
            dirtyHashes.insert(sub);
        }
        dirtyHashes.insert(name);
        if (fCheckTakeover)
            namesToCheckForTakeover.insert(name);
    }
    CClaimTrieNode* rootNode = &(base->root);
    cachedNode = cache.find("");
    if (cachedNode != cache.end())
        rootNode = cachedNode->second;
    return recursivePruneName(rootNode, 0, name);
}

bool CClaimTrieCache::recursivePruneName(CClaimTrieNode* tnCurrent, unsigned int nPos, std::string sName, bool* pfNullified)
{
    // Recursively prune leaf node(s) without any claims in it and store
    // the modified nodes in the cache

    bool fNullified = false;
    std::string sCurrentSubstring = sName.substr(0, nPos);
    if (nPos < sName.size())
    {
        std::string sNextSubstring = sName.substr(0, nPos + 1);
        unsigned char cNext = sName.at(nPos);
        CClaimTrieNode* tnNext = NULL;
        nodeCacheType::iterator cachedNode = cache.find(sNextSubstring);
        if (cachedNode != cache.end())
            tnNext = cachedNode->second;
        else
        {
            nodeMapType::iterator childNode = tnCurrent->children.find(cNext);
            if (childNode != tnCurrent->children.end())
                tnNext = childNode->second;
        }
        if (tnNext == NULL)
            return false;
        bool fChildNullified = false;
        if (!recursivePruneName(tnNext, nPos + 1, sName, &fChildNullified))
            return false;
        if (fChildNullified)
        {
            // If the child nullified itself, the child should already be
            // out of the cache, and the character must now be removed
            // from the current node's map of child nodes to ensure that
            // it isn't found when calculating the merkle hash. But
            // tnCurrent isn't necessarily in the cache. If it's not, it
            // has to be added to the cache, so nothing is changed in the
            // trie. If the current node is added to the cache, however,
            // that does not imply that the parent node must be altered to
            // reflect that its child is now in the cache, since it
            // already has a character in its child map which will be used
            // when calculating the merkle root.

            // First, find out if this node is in the cache.
            cachedNode = cache.find(sCurrentSubstring);
            if (cachedNode == cache.end())
            {
                // it isn't, so make a copy, stick it in the cache,
                // and make it the new current node
                tnCurrent = addNodeToCache(sCurrentSubstring, tnCurrent);
            }
            // erase the character from the current node, which is
            // now guaranteed to be in the cache
            nodeMapType::iterator childNode = tnCurrent->children.find(cNext);
            if (childNode != tnCurrent->children.end())
                tnCurrent->children.erase(childNode);
            else
                return false;
        }
    }
    if (sCurrentSubstring.size() != 0 && tnCurrent->empty())
    {
        // If the current node is in the cache, remove it from there
        nodeCacheType::iterator cachedNode = cache.find(sCurrentSubstring);
        if (cachedNode != cache.end())
        {
            assert(tnCurrent == cachedNode->second);
            delete tnCurrent;
            cache.erase(cachedNode);
        }
        fNullified = true;
    }
    if (pfNullified)
        *pfNullified = fNullified;
    return true;
}

bool CClaimTrieCache::addClaim(const std::string& name, const COutPoint& outPoint, uint160 claimId, CAmount nAmount, int nHeight)
{
    LogPrintf("%s: name: %s, txhash: %s, nOut: %d, claimId: %s, nAmount: %d, nHeight: %d, nCurrentHeight: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, claimId.GetHex(), nAmount, nHeight, nCurrentHeight);
    assert(nHeight == nCurrentHeight);
    CClaimValue currentClaim;
    int delayForClaim;
    if (getOriginalInfoForName(name, currentClaim) && currentClaim.claimId == claimId)
    {
        LogPrintf("%s: This is an update to a best claim.\n", __func__);
        delayForClaim = 0;
    }
    else
    {
        delayForClaim = getDelayForName(name);
    }
    CClaimValue newClaim(outPoint, claimId, nAmount, nHeight, nHeight + delayForClaim);
    return addClaimToQueues(name, newClaim);
}

bool CClaimTrieCache::undoSpendClaim(const std::string& name, const COutPoint& outPoint, uint160 claimId, CAmount nAmount, int nHeight, int nValidAtHeight)
{
    LogPrintf("%s: name: %s, txhash: %s, nOut: %d, claimId: %s, nAmount: %d, nHeight: %d, nValidAtHeight: %d, nCurrentHeight: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, claimId.GetHex(), nAmount, nHeight, nValidAtHeight, nCurrentHeight);
    CClaimValue claim(outPoint, claimId, nAmount, nHeight, nValidAtHeight);
    if (nValidAtHeight < nCurrentHeight)
    {
        nameOutPointType entry(name, claim.outPoint);
        addToExpirationQueue(claim.nHeight + base->nExpirationTime, entry);
        return insertClaimIntoTrie(name, claim, false);
    }
    else
    {
        return addClaimToQueues(name, claim);
    }
}

template <typename K, typename V>
typename std::map<K, V>::iterator CClaimTrieCache::getQueueCacheRow(const K &key, std::map<K, V> &map, bool createIfNotExists)
{
    typename std::map<K, V>::iterator itQueueRow = map.find(key);
    if (itQueueRow == map.end())
    {
        // Have to make a new row it put in the cache, if createIfNotExists is true
        V queueRow;
        // If the row exists in the base, copy its claims into the new row.
        bool exists = base->db.getQueueRow(key, queueRow);
        if (!exists && !createIfNotExists)
            return itQueueRow;
        // Stick the new row in the cache
        itQueueRow = map.insert(itQueueRow, std::make_pair(key, queueRow));
    }
    return itQueueRow;
}

bool CClaimTrieCache::addClaimToQueues(const std::string& name, CClaimValue& claim)
{
    LogPrintf("%s: nValidAtHeight: %d\n", __func__, claim.nValidAtHeight);
    claimQueueEntryType entry(name, claim);
    claimQueueType::iterator itQueueRow = getQueueCacheRow(claim.nValidAtHeight, claimQueueCache, true);
    queueNameType::iterator itQueueNameRow = getQueueCacheRow(name, claimQueueNameCache, true);
    itQueueRow->second.push_back(entry);
    itQueueNameRow->second.push_back(outPointHeightType(claim.outPoint, claim.nValidAtHeight));
    nameOutPointType expireEntry(name, claim.outPoint);
    addToExpirationQueue(claim.nHeight + base->nExpirationTime, expireEntry);
    base->addToClaimIndex(name, claim);
    return true;
}

bool CClaimTrieCache::removeClaimFromQueue(const std::string& name, const COutPoint& outPoint, CClaimValue& claim)
{
    queueNameType::iterator itQueueNameRow = getQueueCacheRow(name, claimQueueNameCache, false);
    if (itQueueNameRow == claimQueueNameCache.end())
    {
        return false;
    }
    queueNameRowType::iterator itQueueName;
    for (itQueueName = itQueueNameRow->second.begin(); itQueueName != itQueueNameRow->second.end(); ++itQueueName)
    {
        if (itQueueName->outPoint == outPoint)
        {
            break;
        }
    }
    if (itQueueName == itQueueNameRow->second.end())
    {
        return false;
    }
    claimQueueType::iterator itQueueRow = getQueueCacheRow(itQueueName->nHeight, claimQueueCache, false);
    if (itQueueRow != claimQueueCache.end())
    {
        claimQueueRowType::iterator itQueue;
        for (itQueue = itQueueRow->second.begin(); itQueue != itQueueRow->second.end(); ++itQueue)
        {
            if (name == itQueue->first && itQueue->second.outPoint == outPoint)
            {
                std::swap(claim, itQueue->second);
                itQueueNameRow->second.erase(itQueueName);
                itQueueRow->second.erase(itQueue);
                base->removeFromClaimIndex(claim);
                return true;
            }
        }
    }
    LogPrintf("%s: An inconsistency was found in the claim queue. Please report this to the developers:\nFound in named queue but not in height queue: name: %s, txid: %s, nOut: %d, nValidAtHeight: %d, current height: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, itQueueName->nHeight, nCurrentHeight);
    return false;
}

bool CClaimTrieCache::undoAddClaim(const std::string& name, const COutPoint& outPoint, int nHeight)
{
    int throwaway;
    return removeClaim(name, outPoint, nHeight, throwaway, false);
}

bool CClaimTrieCache::spendClaim(const std::string& name, const COutPoint& outPoint, int nHeight, int& nValidAtHeight)
{
    return removeClaim(name, outPoint, nHeight, nValidAtHeight, true);
}

bool CClaimTrieCache::removeClaim(const std::string& name, const COutPoint& outPoint, int nHeight, int& nValidAtHeight, bool fCheckTakeover)
{
    LogPrintf("%s: name: %s, txhash: %s, nOut: %s, nHeight: %s, nCurrentHeight: %s\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, nHeight, nCurrentHeight);
    bool removed = false;
    CClaimValue claim;
    if (removeClaimFromQueue(name, outPoint, claim))
    {
        removed = true;
    }
    if (removed == false && removeClaimFromTrie(name, outPoint, claim, fCheckTakeover))
    {
        removed = true;
    }
    if (removed == true)
    {
        nValidAtHeight = claim.nValidAtHeight;
        int expirationHeight = nHeight + base->nExpirationTime;
        removeFromExpirationQueue(name, outPoint, expirationHeight);
    }
    return removed;
}

void CClaimTrieCache::addToExpirationQueue(int nExpirationHeight, nameOutPointType& entry)
{
    expirationQueueType::iterator itQueueRow = getQueueCacheRow(nExpirationHeight, expirationQueueCache, true);
    itQueueRow->second.push_back(entry);
}

void CClaimTrieCache::removeFromExpirationQueue(const std::string& name, const COutPoint& outPoint, int expirationHeight)
{
    expirationQueueType::iterator itQueueRow = getQueueCacheRow(expirationHeight, expirationQueueCache, false);
    expirationQueueRowType::iterator itQueue;
    if (itQueueRow != expirationQueueCache.end())
    {
        for (itQueue = itQueueRow->second.begin(); itQueue != itQueueRow->second.end(); ++itQueue)
        {
            if (name == itQueue->name && outPoint == itQueue->outPoint)
            {
                itQueueRow->second.erase(itQueue);
                break;
            }
        }
    }
}

bool CClaimTrieCache::reorderTrieNode(const std::string& name, bool fCheckTakeover)
{
    assert(base);
    nodeCacheType::iterator cachedNode;
    cachedNode = cache.find(name);
    if (cachedNode == cache.end())
    {
        CClaimTrieNode* currentNode;
        cachedNode = cache.find("");
        if(cachedNode == cache.end())
            currentNode = &(base->root);
        else
            currentNode = cachedNode->second;
        for (std::string::const_iterator itCur = name.begin(); itCur != name.end(); ++itCur)
        {
            std::string sCurrentSubstring(name.begin(), itCur);
            std::string sNextSubstring(name.begin(), itCur + 1);

            cachedNode = cache.find(sNextSubstring);
            if (cachedNode != cache.end())
            {
                currentNode = cachedNode->second;
                continue;
            }
            nodeMapType::iterator childNode = currentNode->children.find(*itCur);
            if (childNode != currentNode->children.end())
            {
                currentNode = childNode->second;
                continue;
            }
            // The node doesn't exist, so it can't be reordered.
            return true;
        }
        currentNode = new CClaimTrieNode(*currentNode);
        std::pair<nodeCacheType::iterator, bool> ret;
        ret = cache.insert(std::pair<std::string, CClaimTrieNode*>(name, currentNode));
        assert(ret.second);
        cachedNode = ret.first;
    }
    bool fChanged = false;
    if (cachedNode->second->claims.empty())
    {
        // Nothing in there to reorder
        return true;
    }
    else
    {
        CClaimValue currentTop = cachedNode->second->claims.front();
        supportMapEntryType node;
        getSupportsForName(name, node);
        cachedNode->second->reorderClaims(node);
        if (cachedNode->second->claims.front() != currentTop)
            fChanged = true;
    }
    if (fChanged)
    {
        for (std::string::const_iterator itCur = name.begin(); itCur != name.end(); ++itCur)
        {
            std::string sub(name.begin(), itCur);
            dirtyHashes.insert(sub);
        }
        dirtyHashes.insert(name);
        if (fCheckTakeover)
            namesToCheckForTakeover.insert(name);
    }
    return true;
}

bool CClaimTrieCache::getSupportsForName(const std::string& name, supportMapEntryType& node) const
{
    supportMapType::const_iterator cachedNode;
    cachedNode = supportCache.find(name);
    if (cachedNode != supportCache.end())
    {
        node = cachedNode->second;
        return true;
    }
    else
    {
        return base->db.getQueueRow(name, node);
    }
}

bool CClaimTrieCache::insertSupportIntoMap(const std::string& name, CSupportValue support, bool fCheckTakeover)
{
    supportMapType::iterator cachedNode;
    // If this node is already in the cache, use that
    cachedNode = supportCache.find(name);
    // If not, copy the one from base if it exists, and use that
    if (cachedNode == supportCache.end())
    {
        supportMapEntryType node;
        base->db.getQueueRow(name, node);
        std::pair<supportMapType::iterator, bool> ret;
        ret = supportCache.insert(std::pair<std::string, supportMapEntryType>(name, node));
        assert(ret.second);
        cachedNode = ret.first;
    }
    cachedNode->second.push_back(support);
    // See if this changed the biggest bid
    return reorderTrieNode(name,  fCheckTakeover);
}

bool CClaimTrieCache::removeSupportFromMap(const std::string& name, const COutPoint& outPoint, CSupportValue& support, bool fCheckTakeover)
{
    supportMapType::iterator cachedNode;
    cachedNode = supportCache.find(name);
    if (cachedNode == supportCache.end())
    {
        supportMapEntryType node;
        if (!base->db.getQueueRow(name, node))
        {
            // clearly, this support does not exist
            return false;
        }
        std::pair<supportMapType::iterator, bool> ret;
        ret = supportCache.insert(std::pair<std::string, supportMapEntryType>(name, node));
        assert(ret.second);
        cachedNode = ret.first;
    }
    supportMapEntryType::iterator itSupport;
    for (itSupport = cachedNode->second.begin(); itSupport != cachedNode->second.end(); ++itSupport)
    {
        if (itSupport->outPoint == outPoint)
        {
            std::swap(support, *itSupport);
            cachedNode->second.erase(itSupport);
            return reorderTrieNode(name, fCheckTakeover);
        }
    }
    LogPrintf("CClaimTrieCache::%s() : asked to remove a support that doesn't exist\n", __func__);
    return false;
}

bool CClaimTrieCache::addSupportToQueues(const std::string& name, CSupportValue& support)
{
    LogPrintf("%s: nValidAtHeight: %d\n", __func__, support.nValidAtHeight);
    supportQueueEntryType entry(name, support);
    supportQueueType::iterator itQueueRow = getQueueCacheRow(support.nValidAtHeight, supportQueueCache, true);
    supportQueueNameType::iterator itQueueNameRow = getQueueCacheRow(name, supportQueueNameCache, true);
    itQueueRow->second.push_back(entry);
    itQueueNameRow->second.push_back(outPointHeightType(support.outPoint, support.nValidAtHeight));
    nameOutPointType expireEntry(name, support.outPoint);
    addSupportToExpirationQueue(support.nHeight + base->nExpirationTime, expireEntry);
    return true;
}

bool CClaimTrieCache::removeSupportFromQueue(const std::string& name, const COutPoint& outPoint, CSupportValue& support)
{
    supportQueueNameType::iterator itQueueNameRow = getQueueCacheRow(name, supportQueueNameCache, false);
    if (itQueueNameRow == supportQueueNameCache.end())
    {
        return false;
    }
    supportQueueNameRowType::iterator itQueueName;
    for (itQueueName = itQueueNameRow->second.begin(); itQueueName != itQueueNameRow->second.end(); ++itQueueName)
    {
        if (itQueueName->outPoint == outPoint)
        {
            break;
        }
    }
    if (itQueueName == itQueueNameRow->second.end())
    {
        return false;
    }
    supportQueueType::iterator itQueueRow = getQueueCacheRow(itQueueName->nHeight, supportQueueCache, false);
    if (itQueueRow != supportQueueCache.end())
    {
        supportQueueRowType::iterator itQueue;
        for (itQueue = itQueueRow->second.begin(); itQueue != itQueueRow->second.end(); ++itQueue)
        {
            if (name == itQueue->first && itQueue->second.outPoint == outPoint)
            {
                std::swap(support, itQueue->second);
                itQueueNameRow->second.erase(itQueueName);
                itQueueRow->second.erase(itQueue);
                return true;
            }
        }
    }
    LogPrintf("%s: An inconsistency was found in the claim queue. Please report this to the developers:\nFound in named support queue but not in height support queue: name: %s, txid: %s, nOut: %d, nValidAtHeight: %d, current height: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, itQueueName->nHeight, nCurrentHeight);
    return false;
}

bool CClaimTrieCache::addSupport(const std::string& name, const COutPoint& outPoint, CAmount nAmount, uint160 supportedClaimId, int nHeight)
{
    LogPrintf("%s: name: %s, txhash: %s, nOut: %d, nAmount: %d, supportedClaimId: %s, nHeight: %d, nCurrentHeight: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, nAmount, supportedClaimId.GetHex(), nHeight, nCurrentHeight);
    assert(nHeight == nCurrentHeight);
    CClaimValue claim;
    int delayForSupport;
    if (getOriginalInfoForName(name, claim) && claim.claimId == supportedClaimId)
    {
        LogPrintf("%s: This is a support to a best claim.\n", __func__);
        delayForSupport = 0;
    }
    else
    {
        delayForSupport = getDelayForName(name);
    }
    CSupportValue support(outPoint, supportedClaimId, nAmount, nHeight, nHeight + delayForSupport);
    return addSupportToQueues(name, support);
}

bool CClaimTrieCache::undoSpendSupport(const std::string& name, const COutPoint& outPoint, uint160 supportedClaimId, CAmount nAmount, int nHeight, int nValidAtHeight)
{
    LogPrintf("%s: name: %s, txhash: %s, nOut: %d, nAmount: %d, supportedClaimId: %s, nHeight: %d, nCurrentHeight: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, nAmount, supportedClaimId.GetHex(), nHeight, nCurrentHeight);
    CSupportValue support(outPoint, supportedClaimId, nAmount, nHeight, nValidAtHeight);
    if (nValidAtHeight < nCurrentHeight)
    {
        nameOutPointType entry(name, support.outPoint);
        addSupportToExpirationQueue(support.nHeight + base->nExpirationTime, entry);
        return insertSupportIntoMap(name, support, false);
    }
    else
    {
        return addSupportToQueues(name, support);
    }
}

bool CClaimTrieCache::removeSupport(const std::string& name, const COutPoint& outPoint, int nHeight, int& nValidAtHeight, bool fCheckTakeover)
{
    bool removed = false;
    CSupportValue support;
    if (removeSupportFromQueue(name, outPoint, support))
        removed = true;
    if (removed == false && removeSupportFromMap(name, outPoint, support, fCheckTakeover))
        removed = true;
    if (removed)
    {
        int expirationHeight = nHeight + base->nExpirationTime;
        removeSupportFromExpirationQueue(name, outPoint, expirationHeight);
        nValidAtHeight = support.nValidAtHeight;
    }
    return removed;
}

void CClaimTrieCache::addSupportToExpirationQueue(int nExpirationHeight, nameOutPointType& entry)
{
    supportExpirationQueueType::iterator itQueueRow = getQueueCacheRow(nExpirationHeight, supportExpirationQueueCache, true);
    itQueueRow->second.push_back(entry);
}

void CClaimTrieCache::removeSupportFromExpirationQueue(const std::string& name, const COutPoint& outPoint, int expirationHeight)
{
    supportExpirationQueueType::iterator itQueueRow = getQueueCacheRow(expirationHeight, supportExpirationQueueCache, false);
    supportExpirationQueueRowType::iterator itQueue;
    if (itQueueRow != supportExpirationQueueCache.end())
    {
        for (itQueue = itQueueRow->second.begin(); itQueue != itQueueRow->second.end(); ++itQueue)
        {
            if (name == itQueue->name && outPoint == itQueue->outPoint)
            {
                itQueueRow->second.erase(itQueue);
                break;
            }
        }
    }
}

bool CClaimTrieCache::undoAddSupport(const std::string& name, const COutPoint& outPoint, int nHeight)
{
    LogPrintf("%s: name: %s, txhash: %s, nOut: %d, nHeight: %d, nCurrentHeight: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, nHeight, nCurrentHeight);
    int throwaway;
    return removeSupport(name, outPoint, nHeight, throwaway, false);
}

bool CClaimTrieCache::spendSupport(const std::string& name, const COutPoint& outPoint, int nHeight, int& nValidAtHeight)
{
    LogPrintf("%s: name: %s, txhash: %s, nOut: %d, nHeight: %d, nCurrentHeight: %d\n", __func__, name, outPoint.hash.GetHex(), outPoint.n, nHeight, nCurrentHeight);
    return removeSupport(name, outPoint, nHeight, nValidAtHeight, true);
}

bool CClaimTrieCache::incrementBlock(insertUndoType& insertUndo, claimQueueRowType& expireUndo, insertUndoType& insertSupportUndo, supportQueueRowType& expireSupportUndo, std::vector<std::pair<std::string, int> >& takeoverHeightUndo)
{
    LogPrintf("%s: nCurrentHeight (before increment): %d\n", __func__, nCurrentHeight);

    claimQueueType::iterator itQueueRow = getQueueCacheRow(nCurrentHeight, claimQueueCache, false);
    if (itQueueRow != claimQueueCache.end())
    {
        for (claimQueueRowType::iterator itEntry = itQueueRow->second.begin(); itEntry != itQueueRow->second.end(); ++itEntry)
        {
            bool found = false;
            queueNameType::iterator itQueueNameRow = getQueueCacheRow(itEntry->first, claimQueueNameCache, false);
            if (itQueueNameRow != claimQueueNameCache.end())
            {
                for (queueNameRowType::iterator itQueueName = itQueueNameRow->second.begin(); itQueueName != itQueueNameRow->second.end(); ++itQueueName)
                {
                    if (itQueueName->outPoint == itEntry->second.outPoint && itQueueName->nHeight == nCurrentHeight)
                    {
                        found = true;
                        itQueueNameRow->second.erase(itQueueName);
                        break;
                    }
                }
            }
            if (!found)
            {
                LogPrintf("%s: An inconsistency was found in the claim queue. Please report this to the developers:\nFound in height queue but not in named queue: name: %s, txid: %s, nOut: %d, nValidAtHeight: %d, current height: %d\n", __func__, itEntry->first, itEntry->second.outPoint.hash.GetHex(), itEntry->second.outPoint.n, itEntry->second.nValidAtHeight, nCurrentHeight);
                if (itQueueNameRow != claimQueueNameCache.end())
                {
                    LogPrintf("Claims found for that name:\n");
                    for (queueNameRowType::iterator itQueueName = itQueueNameRow->second.begin(); itQueueName != itQueueNameRow->second.end(); ++itQueueName)
                    {
                        LogPrintf("\ttxid: %s, nOut: %d, nValidAtHeight: %d\n", itQueueName->outPoint.hash.GetHex(), itQueueName->outPoint.n, itQueueName->nHeight);
                    }
                }
                else
                {
                    LogPrintf("No claims found for that name\n");
                }
            }
            assert(found);
            insertClaimIntoTrie(itEntry->first, itEntry->second, true);
            insertUndo.push_back(nameOutPointHeightType(itEntry->first, itEntry->second.outPoint, itEntry->second.nValidAtHeight));
        }
        itQueueRow->second.clear();
    }
    expirationQueueType::iterator itExpirationRow = getQueueCacheRow(nCurrentHeight, expirationQueueCache, false);
    if (itExpirationRow != expirationQueueCache.end())
    {
        for (expirationQueueRowType::iterator itEntry = itExpirationRow->second.begin(); itEntry != itExpirationRow->second.end(); ++itEntry)
        {
            CClaimValue claim;
            assert(removeClaimFromTrie(itEntry->name, itEntry->outPoint, claim, true));
            expireUndo.push_back(std::make_pair(itEntry->name, claim));
            LogPrintf("Expiring claim %s: %s, nHeight: %d, nValidAtHeight: %d\n", claim.claimId.GetHex(), itEntry->name, claim.nHeight, claim.nValidAtHeight);
        }
        itExpirationRow->second.clear();
    }
    supportQueueType::iterator itSupportRow = getQueueCacheRow(nCurrentHeight, supportQueueCache, false);
    if (itSupportRow != supportQueueCache.end())
    {
        for (supportQueueRowType::iterator itSupport = itSupportRow->second.begin(); itSupport != itSupportRow->second.end(); ++itSupport)
        {
            bool found = false;
            supportQueueNameType::iterator itSupportNameRow = getQueueCacheRow(itSupport->first, supportQueueNameCache, false);
            if (itSupportNameRow != supportQueueNameCache.end())
            {
                for (supportQueueNameRowType::iterator itSupportName = itSupportNameRow->second.begin(); itSupportName != itSupportNameRow->second.end(); ++itSupportName)
                {
                    if (itSupportName->outPoint == itSupport->second.outPoint && itSupportName->nHeight == itSupport->second.nValidAtHeight)
                    {
                        found = true;
                        itSupportNameRow->second.erase(itSupportName);
                        break;
                    }
                }
            }
            if (!found)
            {
                LogPrintf("%s: An inconsistency was found in the support queue. Please report this to the developers:\nFound in height queue but not in named queue: %s, txid: %s, nOut: %d, nValidAtHeight: %d, current height: %d\n", __func__, itSupport->first, itSupport->second.outPoint.hash.GetHex(), itSupport->second.outPoint.n, itSupport->second.nValidAtHeight, nCurrentHeight);
                if (itSupportNameRow != supportQueueNameCache.end())
                {
                    LogPrintf("Supports found for that name:\n");
                    for (supportQueueNameRowType::iterator itSupportName = itSupportNameRow->second.begin(); itSupportName != itSupportNameRow->second.end(); ++itSupportName)
                    {
                        LogPrintf("\ttxid: %s, nOut: %d, nValidAtHeight: %d\n", itSupportName->outPoint.hash.GetHex(), itSupportName->outPoint.n, itSupportName->nHeight);
                    }
                }
                else
                {
                    LogPrintf("No support found for that name\n");
                }
            }
            insertSupportIntoMap(itSupport->first, itSupport->second, true);
            insertSupportUndo.push_back(nameOutPointHeightType(itSupport->first, itSupport->second.outPoint, itSupport->second.nValidAtHeight));
        }
        itSupportRow->second.clear();
    }
    supportExpirationQueueType::iterator itSupportExpirationRow = getQueueCacheRow(nCurrentHeight, supportExpirationQueueCache, false);
    if (itSupportExpirationRow != supportExpirationQueueCache.end())
    {
        for (supportExpirationQueueRowType::iterator itEntry = itSupportExpirationRow->second.begin(); itEntry != itSupportExpirationRow->second.end(); ++itEntry)
        {
            CSupportValue support;
            assert(removeSupportFromMap(itEntry->name, itEntry->outPoint, support, true));
            expireSupportUndo.push_back(std::make_pair(itEntry->name, support));
            LogPrintf("Expiring support %s: %s, nHeight: %d, nValidAtHeight: %d\n", support.supportedClaimId.GetHex(), itEntry->name, support.nHeight, support.nValidAtHeight);
        }
        itSupportExpirationRow->second.clear();
    }
    // check each potentially taken over name to see if a takeover occurred.
    // if it did, then check the claim and support insertion queues for
    // the names that have been taken over, immediately insert all claim and
    // supports for those names, and stick them in the insertUndo or
    // insertSupportUndo vectors, with the nValidAtHeight they had prior to
    // this block.
    // Run through all names that have been taken over
    for (std::set<std::string>::iterator itNamesToCheck = namesToCheckForTakeover.begin(); itNamesToCheck != namesToCheckForTakeover.end(); ++itNamesToCheck)
    {
        // Check if a takeover has occurred
        nodeCacheType::iterator itCachedNode = cache.find(*itNamesToCheck);
        // many possibilities
        // if this node is new, don't put it into the undo -- there will be nothing to restore, after all
        // if all of this node's claims were deleted, it should be put into the undo -- there could be
        // claims in the queue for that name and the takeover height should be the current height
        // if the node is not in the cache, or getbestclaim fails, that means all of its claims were
        // deleted
        // if getOriginalInfoForName returns false, that means it's new and shouldn't go into the undo
        // if both exist, and the current best claim is not the same as or the parent to the new best
        // claim, then ownership has changed and the current height of last takeover should go into
        // the queue
        CClaimValue claimInCache;
        CClaimValue claimInTrie;
        bool haveClaimInCache;
        bool haveClaimInTrie;
        if (itCachedNode == cache.end())
        {
            haveClaimInCache = false;
        }
        else
        {
            haveClaimInCache = itCachedNode->second->getBestClaim(claimInCache);
        }
        haveClaimInTrie = getOriginalInfoForName(*itNamesToCheck, claimInTrie);
        bool takeoverHappened = false;
        if (!haveClaimInTrie)
        {
            takeoverHappened = true;
        }
        else if (!haveClaimInCache)
        {
            takeoverHappened = true;
        }
        else if (claimInCache != claimInTrie)
        {
            if (claimInCache.claimId != claimInTrie.claimId)
            {
                takeoverHappened = true;
            }
        }
        if (takeoverHappened)
        {
            // Get all claims in the queue for that name
            queueNameType::iterator itQueueNameRow = getQueueCacheRow(*itNamesToCheck, claimQueueNameCache, false);
            if (itQueueNameRow != claimQueueNameCache.end())
            {
                for (queueNameRowType::iterator itQueueName = itQueueNameRow->second.begin(); itQueueName != itQueueNameRow->second.end(); ++itQueueName)
                {
                    bool found = false;
                    // Pull those claims out of the height-based queue
                    claimQueueType::iterator itQueueRow = getQueueCacheRow(itQueueName->nHeight, claimQueueCache, false);
                    claimQueueRowType::iterator itQueue;
                    if (itQueueRow != claimQueueCache.end())
                    {
                        for (itQueue = itQueueRow->second.begin(); itQueue != itQueueRow->second.end(); ++itQueue)
                        {
                            if (*itNamesToCheck == itQueue->first && itQueue->second.outPoint == itQueueName->outPoint && itQueue->second.nValidAtHeight == itQueueName->nHeight)
                            {
                                found = true;
                                break;
                            }
                        }
                    }
                    if (found)
                    {
                        // Insert them into the queue undo with their previous nValidAtHeight
                        insertUndo.push_back(nameOutPointHeightType(itQueue->first, itQueue->second.outPoint, itQueue->second.nValidAtHeight));
                        // Insert them into the name trie with the new nValidAtHeight
                        itQueue->second.nValidAtHeight = nCurrentHeight;
                        insertClaimIntoTrie(itQueue->first, itQueue->second, false);
                        // Delete them from the height-based queue
                        itQueueRow->second.erase(itQueue);
                    }
                    else
                    {
                        LogPrintf("%s(): An inconsistency was found in the claim queue. Please report this to the developers:\nClaim found in name queue but not in height based queue:\nname: %s, txid: %s, nOut: %d, nValidAtHeight in name based queue: %d, current height: %d\n", __func__, *itNamesToCheck, itQueueName->outPoint.hash.GetHex(), itQueueName->outPoint.n, itQueueName->nHeight, nCurrentHeight);
                    }
                    assert(found);
                }
                // remove all claims from the queue for that name
                itQueueNameRow->second.clear();
            }
            //
            // Then, get all supports in the queue for that name
            supportQueueNameType::iterator itSupportQueueNameRow = getQueueCacheRow(*itNamesToCheck, supportQueueNameCache, false);
            if (itSupportQueueNameRow != supportQueueNameCache.end())
            {
                for (supportQueueNameRowType::iterator itSupportQueueName = itSupportQueueNameRow->second.begin(); itSupportQueueName != itSupportQueueNameRow->second.end(); ++itSupportQueueName)
                {
                    // Pull those supports out of the height-based queue
                    supportQueueType::iterator itSupportQueueRow = getQueueCacheRow(itSupportQueueName->nHeight, supportQueueCache, false);
                    if (itSupportQueueRow != supportQueueCache.end())
                    {
                        supportQueueRowType::iterator itSupportQueue;
                        for (itSupportQueue = itSupportQueueRow->second.begin(); itSupportQueue != itSupportQueueRow->second.end(); ++itSupportQueue)
                        {
                            if (*itNamesToCheck == itSupportQueue->first && itSupportQueue->second.outPoint == itSupportQueueName->outPoint && itSupportQueue->second.nValidAtHeight == itSupportQueueName->nHeight)
                            {
                                break;
                            }
                        }
                        if (itSupportQueue != itSupportQueueRow->second.end())
                        {
                            // Insert them into the support queue undo with the previous nValidAtHeight
                            insertSupportUndo.push_back(nameOutPointHeightType(itSupportQueue->first, itSupportQueue->second.outPoint, itSupportQueue->second.nValidAtHeight));
                            // Insert them into the support map with the new nValidAtHeight
                            itSupportQueue->second.nValidAtHeight = nCurrentHeight;
                            insertSupportIntoMap(itSupportQueue->first, itSupportQueue->second, false);
                            // Delete them from the height-based queue
                            itSupportQueueRow->second.erase(itSupportQueue);
                        }
                        else
                        {
                            // here be problems TODO: show error, assert false
                        }
                    }
                    else
                    {
                        // here be problems
                    }
                }
                // remove all supports from the queue for that name
                itSupportQueueNameRow->second.clear();
            }

            // save the old last height so that it can be restored if the block is undone
            if (haveClaimInTrie)
            {
                int nHeightOfLastTakeover;
                assert(getLastTakeoverForName(*itNamesToCheck, nHeightOfLastTakeover));
                takeoverHeightUndo.push_back(std::make_pair(*itNamesToCheck, nHeightOfLastTakeover));
            }
            itCachedNode = cache.find(*itNamesToCheck);
            if (itCachedNode != cache.end())
            {
                cacheTakeoverHeights[*itNamesToCheck] = nCurrentHeight;
            }
        }
    }
    for (nodeCacheType::const_iterator itOriginals = block_originals.begin(); itOriginals != block_originals.end(); ++itOriginals)
    {
        delete itOriginals->second;
    }
    block_originals.clear();
    for (nodeCacheType::const_iterator itCache = cache.begin(); itCache != cache.end(); ++itCache)
    {
        block_originals[itCache->first] = new CClaimTrieNode(*(itCache->second));
    }
    namesToCheckForTakeover.clear();
    nCurrentHeight++;
    return true;
}

bool CClaimTrieCache::decrementBlock(insertUndoType& insertUndo, claimQueueRowType& expireUndo, insertUndoType& insertSupportUndo, supportQueueRowType& expireSupportUndo, std::vector<std::pair<std::string, int> >& takeoverHeightUndo)
{
    LogPrintf("%s: nCurrentHeight (before decrement): %d\n", __func__, nCurrentHeight);
    nCurrentHeight--;

    if (expireSupportUndo.begin() != expireSupportUndo.end())
    {
        supportExpirationQueueType::iterator itSupportExpireRow = getQueueCacheRow(nCurrentHeight, supportExpirationQueueCache, true);
        for (supportQueueRowType::iterator itSupportExpireUndo = expireSupportUndo.begin(); itSupportExpireUndo != expireSupportUndo.end(); ++itSupportExpireUndo)
        {
            insertSupportIntoMap(itSupportExpireUndo->first, itSupportExpireUndo->second, false);
            itSupportExpireRow->second.push_back(nameOutPointType(itSupportExpireUndo->first, itSupportExpireUndo->second.outPoint));
        }
    }

    for (insertUndoType::iterator itSupportUndo = insertSupportUndo.begin(); itSupportUndo != insertSupportUndo.end(); ++itSupportUndo)
    {
        supportQueueType::iterator itSupportRow = getQueueCacheRow(itSupportUndo->nHeight, supportQueueCache, true);
        CSupportValue support;
        assert(removeSupportFromMap(itSupportUndo->name, itSupportUndo->outPoint, support, false));
        supportQueueNameType::iterator itSupportNameRow = getQueueCacheRow(itSupportUndo->name, supportQueueNameCache, true);
        itSupportRow->second.push_back(std::make_pair(itSupportUndo->name, support));
        itSupportNameRow->second.push_back(outPointHeightType(support.outPoint, support.nValidAtHeight));
    }

    if (expireUndo.begin() != expireUndo.end())
    {
        expirationQueueType::iterator itExpireRow = getQueueCacheRow(nCurrentHeight, expirationQueueCache, true);
        for (claimQueueRowType::iterator itExpireUndo = expireUndo.begin(); itExpireUndo != expireUndo.end(); ++itExpireUndo)
        {
            insertClaimIntoTrie(itExpireUndo->first, itExpireUndo->second, false);
            itExpireRow->second.push_back(nameOutPointType(itExpireUndo->first, itExpireUndo->second.outPoint));
        }
    }

    for (insertUndoType::iterator itInsertUndo = insertUndo.begin(); itInsertUndo != insertUndo.end(); ++itInsertUndo)
    {
        claimQueueType::iterator itQueueRow = getQueueCacheRow(itInsertUndo->nHeight, claimQueueCache, true);
        CClaimValue claim;
        assert(removeClaimFromTrie(itInsertUndo->name, itInsertUndo->outPoint, claim, false));
        queueNameType::iterator itQueueNameRow = getQueueCacheRow(itInsertUndo->name, claimQueueNameCache, true);
        itQueueRow->second.push_back(std::make_pair(itInsertUndo->name, claim));
        itQueueNameRow->second.push_back(outPointHeightType(itInsertUndo->outPoint, itInsertUndo->nHeight));
    }

    for (std::vector<std::pair<std::string, int> >::iterator itTakeoverHeightUndo = takeoverHeightUndo.begin(); itTakeoverHeightUndo != takeoverHeightUndo.end(); ++itTakeoverHeightUndo)
    {
        cacheTakeoverHeights[itTakeoverHeightUndo->first] = itTakeoverHeightUndo->second;
    }

    return true;
}

bool CClaimTrieCache::finalizeDecrement()
{
    for (nodeCacheType::iterator itOriginals = block_originals.begin(); itOriginals != block_originals.end(); ++itOriginals)
    {
        delete itOriginals->second;
    }
    block_originals.clear();
    for (nodeCacheType::const_iterator itCache = cache.begin(); itCache != cache.end(); ++itCache)
    {
        block_originals[itCache->first] = new CClaimTrieNode(*(itCache->second));
    }
    return true;
}

bool CClaimTrieCache::getLastTakeoverForName(const std::string& name, int& nLastTakeoverForName) const
{
    if (!fRequireTakeoverHeights)
    {
        nLastTakeoverForName = 0;
        return true;
    }
    std::map<std::string, int>::const_iterator itHeights = cacheTakeoverHeights.find(name);
    if (itHeights == cacheTakeoverHeights.end())
    {
        return base->getLastTakeoverForName(name, nLastTakeoverForName);
    }
    nLastTakeoverForName = itHeights->second;
    return true;
}

int CClaimTrieCache::getNumBlocksOfContinuousOwnership(const std::string& name) const
{
    const CClaimTrieNode* node = NULL;
    nodeCacheType::const_iterator itCache = cache.find(name);
    if (itCache != cache.end())
    {
        node = itCache->second;
    }
    if (!node)
    {
        node = base->getNodeForName(name);
    }
    if (!node || node->claims.empty())
    {
        return 0;
    }
    int nLastTakeoverHeight;
    assert(getLastTakeoverForName(name, nLastTakeoverHeight));
    return nCurrentHeight - nLastTakeoverHeight;
}

int CClaimTrieCache::getDelayForName(const std::string& name) const
{
    if (!fRequireTakeoverHeights)
    {
        return 0;
    }
    int nBlocksOfContinuousOwnership = getNumBlocksOfContinuousOwnership(name);
    return std::min(nBlocksOfContinuousOwnership / base->nProportionalDelayFactor, 4032);
}

uint256 CClaimTrieCache::getBestBlock() const
{
    return hashBlock;
}

void CClaimTrieCache::setBestBlock(const uint256& hashBlockIn)
{
    hashBlock = hashBlockIn;
}

bool CClaimTrieCache::clear()
{
    for (nodeCacheType::iterator itcache = cache.begin(); itcache != cache.end(); ++itcache)
    {
        delete itcache->second;
    }
    cache.clear();
    for (nodeCacheType::iterator itOriginals = block_originals.begin(); itOriginals != block_originals.end(); ++itOriginals)
    {
        delete itOriginals->second;
    }
    block_originals.clear();
    dirtyHashes.clear();
    cacheHashes.clear();
    claimQueueCache.clear();
    claimQueueNameCache.clear();
    expirationQueueCache.clear();
    supportCache.clear();
    supportQueueCache.clear();
    supportQueueNameCache.clear();
    supportExpirationQueueCache.clear();
    namesToCheckForTakeover.clear();
    cacheTakeoverHeights.clear();
    return true;
}

template <typename K, typename V> void CClaimTrieCache::update(std::map<K, V> &map)
{
    for (typename std::map<K, V>::iterator itQueue = map.begin(); itQueue != map.end(); ++itQueue)
    {
        base->db.updateQueueRow(itQueue->first, itQueue->second);
    }
}

bool CClaimTrieCache::flush()
{
    if (dirty())
        getMerkleHash();

    for (nodeCacheType::iterator itcache = cache.begin(); itcache != cache.end(); ++itcache)
    {
        if (!base->updateName(itcache->first, itcache->second))
        {
            LogPrintf("%s: Failed to update name for:%s\n", __func__, itcache->first);
            return false;
        }
    }
    for (hashMapType::iterator ithash = cacheHashes.begin(); ithash != cacheHashes.end(); ++ithash)
    {
        if (!base->updateHash(ithash->first, ithash->second))
        {
            LogPrintf("%s: Failed to update hash for:%s\n", __func__, ithash->first);
            return false;
        }
    }
    for (std::map<std::string, int>::iterator itheight = cacheTakeoverHeights.begin(); itheight != cacheTakeoverHeights.end(); ++itheight)
    {
        if (!base->updateTakeoverHeight(itheight->first, itheight->second))
        {
            LogPrintf("%s: Failed to update takeover height for:%s\n", __func__, itheight->first);
            return false;
        }
    }
    update(claimQueueCache);
    update(claimQueueNameCache);
    update(expirationQueueCache);
    update(supportCache);
    update(supportQueueCache);
    update(supportQueueNameCache);
    update(supportExpirationQueueCache);
    base->hashBlock = getBestBlock();
    base->nCurrentHeight = nCurrentHeight;
    return clear();
}

bool CClaimTrieCache::dirty() const
{
    return !dirtyHashes.empty();
}

uint256 CClaimTrieCache::getLeafHashForProof(const std::string& currentPosition, unsigned char nodeChar, const CClaimTrieNode* currentNode) const
{
    std::stringstream leafPosition;
    leafPosition << currentPosition << nodeChar;
    hashMapType::const_iterator cachedHash = cacheHashes.find(leafPosition.str());
    if (cachedHash != cacheHashes.end())
    {
        return cachedHash->second;
    }
    else
    {
        return currentNode->hash;
    }
}

CClaimTrieProof CClaimTrieCache::getProofForName(const std::string& name) const
{
    if (dirty())
        getMerkleHash();
    std::vector<CClaimTrieProofNode> nodes;
    CClaimTrieNode* current = &(base->root);
    nodeCacheType::const_iterator cachedNode;
    bool fNameHasValue = false;
    COutPoint outPoint;
    int nHeightOfLastTakeover = 0;
    for (std::string::const_iterator itName = name.begin(); current; ++itName)
    {
        std::string currentPosition(name.begin(), itName);
        cachedNode = cache.find(currentPosition);
        if (cachedNode != cache.end())
            current = cachedNode->second;
        CClaimValue claim;
        bool fNodeHasValue = current->getBestClaim(claim);
        uint256 valueHash;
        if (fNodeHasValue)
        {
            int nHeightOfLastTakeover;
            assert(getLastTakeoverForName(currentPosition, nHeightOfLastTakeover));
            valueHash = getValueHash(claim.outPoint, nHeightOfLastTakeover);
        }
        std::vector<std::pair<unsigned char, uint256> > children;
        CClaimTrieNode* nextCurrent = NULL;
        for (nodeMapType::const_iterator itChildren = current->children.begin(); itChildren != current->children.end(); ++itChildren)
        {
            if (itName == name.end() || itChildren->first != *itName) // Leaf node
            {
                uint256 childHash = getLeafHashForProof(currentPosition, itChildren->first, itChildren->second);
                children.push_back(std::make_pair(itChildren->first, childHash));
            }
            else // Full node
            {
                nextCurrent = itChildren->second;
                uint256 childHash;
                children.push_back(std::make_pair(itChildren->first, childHash));
            }
        }
        if (currentPosition == name)
        {
            fNameHasValue = fNodeHasValue;
            if (fNameHasValue)
            {
                outPoint = claim.outPoint;
                assert(getLastTakeoverForName(name, nHeightOfLastTakeover));
            }
            valueHash.SetNull();
        }
        CClaimTrieProofNode node(children, fNodeHasValue, valueHash);
        nodes.push_back(node);
        current = nextCurrent;
    }
    return CClaimTrieProof(nodes, fNameHasValue, outPoint,
                           nHeightOfLastTakeover);
}

void CClaimTrieCache::removeAndAddToExpirationQueue(expirationQueueRowType &row, int height, bool increment)
{
    for (expirationQueueRowType::iterator e = row.begin(); e != row.end(); ++e)
    {
        // remove and insert with new expiration time
        removeFromExpirationQueue(e->name, e->outPoint, height);
        int extend_expiration = Params().GetConsensus().nExtendedClaimExpirationTime - Params().GetConsensus().nOriginalClaimExpirationTime;
        int new_expiration_height = increment ? height + extend_expiration : height - extend_expiration;
        nameOutPointType entry(e->name, e->outPoint);
        addToExpirationQueue(new_expiration_height, entry);
    }
}

void CClaimTrieCache::removeAndAddSupportToExpirationQueue(supportExpirationQueueRowType &row, int height, bool increment)
{
    for (supportExpirationQueueRowType::iterator e = row.begin(); e != row.end(); ++e)
    {
        // remove and insert with new expiration time
        removeSupportFromExpirationQueue(e->name, e->outPoint, height);
        int extend_expiration = Params().GetConsensus().nExtendedClaimExpirationTime - Params().GetConsensus().nOriginalClaimExpirationTime;
        int new_expiration_height = increment ? height + extend_expiration : height - extend_expiration;
        nameOutPointType entry(e->name, e->outPoint);
        addSupportToExpirationQueue(new_expiration_height, entry);
    }
}

bool CClaimTrieCache::forkForExpirationChange(bool increment)
{
    /*
    If increment is True, we have forked to extend the expiration time, thus items in the expiration queue
    will have their expiration extended by "new expiration time - original expiration time"

    If increment is False, we are decremented a block to reverse the fork. Thus items in the expiration queue
    will have their expiration extension removed.
    */

    expirationQueueType dirtyExpirationQueueRows;
    if (!base->db.getQueueMap(dirtyExpirationQueueRows))
    {
        return error("%s(): error reading expiration queue rows from disk", __func__);
    }

    supportExpirationQueueType dirtySupportExpirationQueueRows;
    if (!base->db.getQueueMap(dirtySupportExpirationQueueRows))
    {
        return error("%s(): error reading support expiration queue rows from disk", __func__);
    }

    for (expirationQueueType::const_iterator i = dirtyExpirationQueueRows.begin(); i != dirtyExpirationQueueRows.end(); ++i)
    {
        int height = i->first;
        expirationQueueRowType row = i->second;
        removeAndAddToExpirationQueue(row, height, increment);
    }

    for (supportExpirationQueueType::const_iterator i = dirtySupportExpirationQueueRows.begin(); i != dirtySupportExpirationQueueRows.end(); ++i)
    {
        int height = i->first;
        supportExpirationQueueRowType row = i->second;
        removeAndAddSupportToExpirationQueue(row, height, increment);
    }

    return true;
}
