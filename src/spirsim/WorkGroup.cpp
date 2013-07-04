#include "common.h"

#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS
#include "llvm/Constants.h"
#include "llvm/DebugInfo.h"
#include "llvm/InstrTypes.h"
#include "llvm/Instruction.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Type.h"

#include "Kernel.h"
#include "Memory.h"
#include "WorkGroup.h"
#include "WorkItem.h"

using namespace spirsim;
using namespace std;

WorkGroup::WorkGroup(const Kernel& kernel, Memory& globalMem,
                     unsigned int workDim,
                     size_t wgid_x, size_t wgid_y, size_t wgid_z,
                     const size_t globalSize[3],
                     const size_t groupSize[3])
  : m_globalMemory(globalMem)
{
  m_workDim = workDim;
  m_groupID[0] = wgid_x;
  m_groupID[1] = wgid_y;
  m_groupID[2] = wgid_z;
  m_globalSize[0] = globalSize[0];
  m_globalSize[1] = globalSize[1];
  m_globalSize[2] = globalSize[2];
  m_groupSize[0] = groupSize[0];
  m_groupSize[1] = groupSize[1];
  m_groupSize[2] = groupSize[2];

  // Allocate local memory
  m_localMemory = kernel.getLocalMemory()->clone();

  // Initialise work-items
  m_totalWorkItems = groupSize[0] * groupSize[1] * groupSize[2];
  m_workItems = new WorkItem*[m_totalWorkItems];
  for (size_t k = 0; k < groupSize[2]; k++)
  {
    for (size_t j = 0; j < groupSize[1]; j++)
    {
      for (size_t i = 0; i < groupSize[0]; i++)
      {
        WorkItem *workItem = new WorkItem(*this, kernel, globalMem, i, j, k);
        m_workItems[i + (j + k*groupSize[1])*groupSize[0]] = workItem;
      }
    }
  }

  m_nextEvent = 1;
}

WorkGroup::~WorkGroup()
{
  // Delete work-items
  for (int i = 0; i < m_totalWorkItems; i++)
  {
    delete m_workItems[i];
  }
  delete[] m_workItems;

  delete m_localMemory;
}

uint64_t WorkGroup::async_copy(AsyncCopy copy, uint64_t event)
{
  // TODO: Ensure all work-items hit same async_copy at same time?
  map< uint64_t, list<AsyncCopy> >::iterator eItr;
  for (eItr = m_pendingEvents.begin(); eItr != m_pendingEvents.end(); eItr++)
  {
    list<AsyncCopy>::iterator cItr;
    for (cItr = eItr->second.begin(); cItr != eItr->second.end(); cItr++)
    {
      if (*cItr == copy)
      {
        return eItr->first;
      }
    }
  }

  event = m_nextEvent++;
  m_pendingEvents[event] = list<AsyncCopy>();
  m_pendingEvents[event].push_back(copy);

  return event;
}

void WorkGroup::dumpLocalMemory() const
{
  if (m_localMemory->getTotalAllocated() > 0)
  {
    cout << SMALL_SEPARATOR << endl << "Local Memory:";
    m_localMemory->dump();
  }
}

void WorkGroup::dumpPrivateMemory() const
{
  for (int i = 0; i < m_totalWorkItems; i++)
  {
    cout << SMALL_SEPARATOR;
    m_workItems[i]->dumpPrivateMemory();
  }
}

const size_t* WorkGroup::getGlobalSize() const
{
  return m_globalSize;
}

const size_t* WorkGroup::getGroupID() const
{
  return m_groupID;
}

const size_t* WorkGroup::getGroupSize() const
{
  return m_groupSize;
}

Memory* WorkGroup::getLocalMemory() const
{
  return m_localMemory;
}

unsigned int WorkGroup::getWorkDim() const
{
  return m_workDim;
}

void WorkGroup::run(const Kernel& kernel, bool outputInstructions)
{
  const llvm::Function *function = kernel.getFunction();

  // Run until all work-items have finished
  int numFinished = 0;
  while (numFinished < m_totalWorkItems)
  {
    // Run work-items in order
    int numBarriers = 0;
    int numWaitEvents = 0;
    for (int i = 0; i < m_totalWorkItems; i++)
    {
      // Check if work-item is ready to execute
      WorkItem *workItem = m_workItems[i];
      if (workItem->getState() != WorkItem::READY)
      {
        continue;
      }

      // Debug output
      if (outputInstructions)
      {
        cout << SMALL_SEPARATOR << endl;
        const size_t *gid = m_workItems[i]->getGlobalID();
        cout << "Work-item ("
             << gid[0] << "," << gid[1] << "," << gid[2]
             << "):" << endl;
      }

      // Run work-item until barrier or complete
      WorkItem::State state = workItem->getState();
      while (state == WorkItem::READY)
      {
        state = workItem->step(outputInstructions);
      }

      // Update counters
      if (state == WorkItem::BARRIER)
      {
        numBarriers++;
        if (outputInstructions)
        {
          cout << SMALL_SEPARATOR << endl;
          cout << "Barrier reached." << endl;
        }
      }
      else if (state == WorkItem::WAIT_EVENT)
      {
        numWaitEvents++;
        if (outputInstructions)
        {
          cout << SMALL_SEPARATOR << endl;
          cout << "Wait for events reached." << endl;
        }
      }
      else if (state == WorkItem::FINISHED)
      {
        numFinished++;
        if (outputInstructions)
        {
          cout << SMALL_SEPARATOR << endl;
          cout << "Kernel completed." << endl;
        }
      }
    }

    // TODO: Handle work-items hitting different barriers
    // Check if all work-items have reached a barrier
    if (numBarriers == m_totalWorkItems)
    {
      for (int i = 0; i < m_totalWorkItems; i++)
      {
        m_workItems[i]->clearBarrier();
      }
      if (outputInstructions)
      {
        cout << "All work-items reached barrier." << endl;
      }
    }
    else if (numBarriers > 0)
    {
      cout << "Barrier divergence detected." << endl;
      return;
    }

    if (numWaitEvents == m_totalWorkItems)
    {
      // Perform group copy
      set<uint64_t>::iterator eItr;
      for (eItr = m_waitEvents.begin(); eItr != m_waitEvents.end(); eItr++)
      {
        list<AsyncCopy> copies = m_pendingEvents[*eItr];
        list<AsyncCopy>::iterator itr;
        for (itr = copies.begin(); itr != copies.end(); itr++)
        {
          Memory *destMem, *srcMem;
          if (itr->type == GLOBAL_TO_LOCAL)
          {
            destMem = m_localMemory;
            srcMem = &m_globalMemory;
          }
          else
          {
            destMem = &m_globalMemory;
            srcMem = m_localMemory;
          }

          // TODO: Strided copies
          unsigned char *buffer = new unsigned char[itr->size];
          // TODO: Check result of load/store and produce error message
          // TODO: Support for direct copying in Memory class
          srcMem->load(buffer, itr->src, itr->size);
          destMem->store(buffer, itr->dest, itr->size);
          delete[] buffer;
        }
        m_pendingEvents.erase(*eItr);
      }
      m_waitEvents.clear();

      for (int i = 0; i < m_totalWorkItems; i++)
      {
        m_workItems[i]->clearBarrier();
      }
      if (outputInstructions)
      {
        cout << "All work-items reached wait for events." << endl;
      }
    }
    else if (numWaitEvents > 0)
    {
      cout << "Wait for events divergence detected." << endl;
      return;
    }
  }

  if (outputInstructions)
  {
    cout << "All work-items completed kernel." << endl;
  }
}

void WorkGroup::wait_event(uint64_t event)
{
  // TODO: Ensure all work-items hit same wait at same time?
  assert(m_pendingEvents.find(event) != m_pendingEvents.end());
  m_waitEvents.insert(event);
}

bool WorkGroup::AsyncCopy::operator==(AsyncCopy copy) const
{
  return
    (instruction == copy.instruction) &&
    (type == copy.type) &&
    (dest == copy.dest) &&
    (src == copy.src) &&
    (size == copy.size);
}
