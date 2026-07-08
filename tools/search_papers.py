import urllib.request, xml.etree.ElementTree as ET

# More targeted: "learned index" OR "spatial index" OR "image retrieval" from 2024-2025
queries = [
    "all:(learned+AND+index+AND+spatial)+AND+all:(image+OR+query)",
    "all:(selectivity+AND+estimation+AND+image)+AND+all:(query+OR+database)",
    "all:(content+based+image+retrieval)+AND+all:(index+OR+database)",
]

for q in queries:
    url = f"http://export.arxiv.org/api/query?search_query={q}&start=0&max_results=8&sortBy=submittedDate&sortOrder=descending"
    try:
        req = urllib.request.Request(url)
        resp = urllib.request.urlopen(req, timeout=15)
        root = ET.fromstring(resp.read().decode())
        ns = {'atom': 'http://www.w3.org/2005/Atom'}
        for entry in root.findall('atom:entry', ns)[:3]:
            title = entry.find('atom:title', ns).text.strip().replace('\n', ' ')
            published = entry.find('atom:published', ns).text[:10]
            aid = entry.find('atom:id', ns).text.split('/')[-1]
            cats = [c.get('term') for c in entry.findall('atom:category', ns)]
            print(f"[{published}] [{', '.join(cats[:3])}] {title[:120]}")
            print(f"    https://arxiv.org/abs/{aid}")
    except Exception as e:
        print(f"Error: {e}")
    print()
