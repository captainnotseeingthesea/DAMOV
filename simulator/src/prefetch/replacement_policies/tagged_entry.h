#ifndef __TAGGED_ENTRY_H__
#define __TAGGED_ENTRY_H__

#include "memory_hierarchy.h"
#include "replaceable_entry.h"

/**
 * A tagged entry is an entry containing a tag.
 * A tagged entry's contents are only relevant if it is marked as valid.
 */
class TaggedEntry : public ReplaceableEntry
{
  public:
    TaggedEntry() : _valid(false), _tag(MaxAddr) {}
    ~TaggedEntry() = default;

    /**
     * Checks if the entry is valid.
     *
     * @return True if the entry is valid.
     */
    virtual bool isValid() const { return _valid; }

    /**
     * Get tag associated to this block.
     *
     * @return The tag value.
     */
    virtual Address getTag() const { return _tag; }

    /**
     * Checks if the given tag information corresponds to this entry's.
     *
     * @param tag The tag value to compare to.
     * @return True if the tag information match this entry's.
     */
    virtual bool
    matchTag(Address tag) const
    {
        return isValid() && (getTag() == tag);
    }

    /**
     * Insert the block by assigning it a tag and marking it valid. Touches
     * block if it hadn't been touched previously.
     *
     * @param tag The tag value.
     */
    virtual void
    insert(const Address tag)
    {
        setValid();
        setTag(tag);
    }

    /** Invalidate the block. Its contents are no longer valid. */
    virtual void invalidate()
    {
        _valid = false;
        setTag(MaxAddr);
    }

  protected:
    /**
     * Set tag associated to this block.
     *
     * @param tag The tag value.
     */
    virtual void setTag(Address tag) { _tag = tag; }

    /** Set valid bit. The block must be invalid beforehand. */
    virtual void
    setValid()
    {
        assert(!isValid());
        _valid = true;
    }

  private:
    /**
     * Valid bit. The contents of this entry are only valid if this bit is set.
     * @sa invalidate()
     * @sa insert()
     */
    bool _valid;

    /** The entry's tag. */
    Address _tag;
};

#endif // __TAGGED_ENTRY_H__