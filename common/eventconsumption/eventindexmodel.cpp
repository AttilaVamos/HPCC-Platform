/*##############################################################################

    Copyright (C) 2025 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "eventindexmodel.hpp"

class CIndexEventModel : public CInterfaceOf<IEventModel>
{
public: // IEventModel
    IMPLEMENT_IEVENTVISITATIONLINK;

    virtual bool visitEvent(CEvent& event) override
    {
        if (!nextLink)
            return false;
        bool propagate = true;
        switch (event.queryType())
        {
        case MetaFileInformation:
            propagate = onMetaFileInformation(event);
            break;
        case EventIndexLoad:
            propagate = onIndexLoad(event);
            break;
        default:
            break;
        }
        return (propagate ? nextLink->visitEvent(event) : true);
    }

public:
    bool configure(const IPropertyTree& config)
    {
        const IPropertyTree* node = config.queryBranch("storage");
        if (!node)
            return false;
        storage.configure(*node);
        return true;
    }

protected:
    bool onMetaFileInformation(CEvent& event)
    {
        storage.observeFile(event.queryNumericValue(EvAttrFileId), event.queryTextValue(EvAttrPath));
        return true;
    }

    bool onIndexLoad(CEvent& event)
    {
        ModeledPage page;
        storage.useAndDescribePage(event.queryNumericValue(EvAttrFileId), event.queryNumericValue(EvAttrFileOffset), event.queryNumericValue(EvAttrNodeKind), page);
        event.setValue(EvAttrReadTime, page.readTime);
        return true;
    }

protected:
    Storage storage;
};

IEventModel* createIndexEventModel(const IPropertyTree& config)
{
    Owned<CIndexEventModel> model = new CIndexEventModel;
    model->configure(config);
    return model.getClear();
}

#ifdef _USE_CPPUNIT

#include "eventunittests.hpp"

class IndexEventModelTests : public CppUnit::TestFixture
{
    // Each use of testEventVisitationLinks() should generally be a separate test case. The test
    // framework will identify the test case for any failure. It will not identify which use of
    // testEventVisitationLinks() failed.

    CPPUNIT_TEST_SUITE(IndexEventModelTests);
    CPPUNIT_TEST(testImplicitNoPageCache);
    CPPUNIT_TEST(testExplicitNoPageCache);
    CPPUNIT_TEST(testPageCacheEviction);
    CPPUNIT_TEST(testNoPageCacheEviction);
    CPPUNIT_TEST_SUITE_END();

public:
    void testImplicitNoPageCache()
    {
        constexpr const char* testData = R"!!!(
            <test>
                <link type="index-events">
                    <storage>
                    <plane name="a" readTime="500"/>
                    </storage>
                </link>
                <input>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="0"/>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="0"/>
                </input>
                <expect>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="500"/>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="500"/>
                </expect>
            </test>
        )!!!";
        testEventVisitationLinks(testData);
    }

    void testExplicitNoPageCache()
    {
        constexpr const char* testData = R"!!!(
        {
            "link": {
                "storage": {
                    "@cache-read": 0,
                    "plane": {
                        "@name": "a",
                        "@readTime": 500
                    }
                },
                "@type": "index-events"
            },
            "input": {
                "event": [
                    {
                        "@type": "IndexLoad",
                        "@FileId": 1,
                        "@FileOffset": 0,
                        "@NodeKind": 0,
                        "@ExpandedSize": 0,
                        "@ExpandTime": 0,
                        "@ReadTime": 0
                    },
                    {
                        "@type": "IndexLoad",
                        "@FileId": 1,
                        "@FileOffset": 0,
                        "@NodeKind": 0,
                        "@ExpandedSize": 0,
                        "@ExpandTime": 0,
                        "@ReadTime": 0
                    }
                ]
            },
            "expect": {
                "event": [
                    {
                        "@type": "IndexLoad",
                        "@FileId": 1,
                        "@FileOffset": 0,
                        "@NodeKind": 0,
                        "@ExpandedSize": 0,
                        "@ExpandTime": 0,
                        "@ReadTime": 500
                    },
                    {
                        "@type": "IndexLoad",
                        "@FileId": 1,
                        "@FileOffset": 0,
                        "@NodeKind": 0,
                        "@ExpandedSize": 0,
                        "@ExpandTime": 0,
                        "@ReadTime": 500
                    }
                ]
            }
        })!!!";
        testEventVisitationLinks(testData);
    }

    void testPageCacheEviction()
    {
        constexpr const char* testData = R"!!!(
link:
- type: index-events
  storage:
    cache-read: 100
    cache-capacity: 8192
    plane:
    - name: a
      readTime: 500
input:
  event:
  - type: IndexLoad
    FileId: 1
    FileOffset: 0
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 0
  - type: IndexLoad
    FileId: 1
    FileOffset: 0
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 0
  - type: IndexLoad
    FileId: 1
    FileOffset: 8192
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 0
  - type: IndexLoad
    FileId: 1
    FileOffset: 8192
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 0
  - type: IndexLoad
    FileId: 1
    FileOffset: 0
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 0
  - type: IndexLoad
    FileId: 1
    FileOffset: 0
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 0
expect:
  event:
  - type: IndexLoad
    FileId: 1
    FileOffset: 0
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 500
  - type: IndexLoad
    FileId: 1
    FileOffset: 0
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 100
  - type: IndexLoad
    FileId: 1
    FileOffset: 8192
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 500
  - type: IndexLoad
    FileId: 1
    FileOffset: 8192
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 100
  - type: IndexLoad
    FileId: 1
    FileOffset: 0
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 500
  - type: IndexLoad
    FileId: 1
    FileOffset: 0
    NodeKind: 0
    ExpandedSize: 0
    ExpandTime: 0
    ReadTime: 100
)!!!";
        testEventVisitationLinks(testData);
    }

    void testNoPageCacheEviction()
    {
        // Same setup as onePageCache with the exception that no eviction occurs
        // because the cache is large enough to hold both pages.
        constexpr const char* testData = R"!!!(
            <test>
                <link type="index-events">
                    <storage cache-read="100" cache-capacity="16384">
                        <plane name="a" readTime="500"/>
                    </storage>
                </link>
                <input>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="0"/>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="0"/>
                    <event type="IndexLoad" FileId="1" FileOffset="8192" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="0"/>
                    <event type="IndexLoad" FileId="1" FileOffset="8192" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="0"/>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="0"/>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="0"/>
                </input>
                <expect>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="500"/>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="100"/>
                    <event type="IndexLoad" FileId="1" FileOffset="8192" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="500"/>
                    <event type="IndexLoad" FileId="1" FileOffset="8192" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="100"/>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="100"/>
                    <event type="IndexLoad" FileId="1" FileOffset="0" NodeKind="0" ExpandedSize="0" ExpandTime="0" ReadTime="100"/>
                </expect>
            </test>
        )!!!";
        testEventVisitationLinks(testData);
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(IndexEventModelTests);
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(IndexEventModelTests, "indexeventmodel");

#endif