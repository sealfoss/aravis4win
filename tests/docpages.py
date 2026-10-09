#!/usr/bin/env python3
# SPDX-License-Identifier:Unlicense
"""Checks the pages of the documentation, in docs/reference/aravis and in the
other content directories that docs/reference/aravis/meson.build passes to
gi-docgen (--content-dir), such as gst/ for the page of the GStreamer plugin.

Usage: docpages.py SOURCE_DIR

Every page, a Markdown file whose first line is a Title: line, must be
published: listed in the content_files of aravis.toml.in, which gi-docgen
renders, and in the expand_content_md_files of meson.build, which make a change
of the page rebuild the documentation. Both lists must name existing pages,
once each.

Every link from a page to a section of a page, as page.html#section or
#section, and every link from the other files of the sources to a section of
the published documentation of the main branch, must name a section that
exists. gi-docgen renders the pages with Python-Markdown, whose toc extension
makes the identifier of a section from its title: this script makes them the
same way, and checks them against Python-Markdown when it is installed. These
identifiers hold letters, digits, '_' and '-' only, so a '.' after a link of
the sources to the published documentation ends a sentence; in a Markdown link,
whose target ends at its ')', a '.' is part of the target.

gi-docgen also generates a page for the API itself, index.html, and one for
each type, function and member, such as class.Camera.html or
method.Camera.uv_set_usb_mode.html. This script does not build the API
documentation: it accepts a link to such a page if the name has the form of
theirs, with any section.

Hidden files, such as the lock files of editors, are not pages, and the files
that cannot be read are left out. Before checking the sources, the script checks
itself on made-up source trees.
"""

import os
import re
import shutil
import sys
import tempfile
import unicodedata

DOCS = os.path.join('docs', 'reference', 'aravis')
# The published documentation of the main branch, as linked from the sources
PUBLISHED_BASE = 'https://aravisproject.github.io/aravis/aravis-' + 'main/'
# A section identifier holds no '.': one after a link ends a sentence
PUBLISHED = re.compile(re.escape(PUBLISHED_BASE) + r'([\w.-]+\.html)(?:#([\w-]+))?')
# A relative link of a page, the whole target of a Markdown link: page.html,
# page.html#section or #section
RELATIVE = re.compile(r'^([\w.-]+\.html)?(?:#([\w.-]+))?$')
# The pages that gi-docgen generates for the API: index.html, the class
# hierarchy, the page of each type, constant and function, and the page of each
# member of a type, whose name may hold '-' for a property or a signal
API_PAGE = re.compile(r'^(?:index|classes_hierarchy'
                      r'|(?:alias|callback|class|const|enum|error|flags|func|iface|struct|union)\.\w+'
                      r'|(?:class_method|ctor|method|property|signal|type_func|vfunc)\.\w+\.[\w-]+)\.html$')
# The files of the sources that may link to the published documentation
SOURCE_EXTENSIONS = ('.c', '.h', '.cc', '.py', '.ps1', '.inf', '.cdf', '.md', '.build', '.txt', '.in', '.yml')
FENCE = re.compile(r'^(```|~~~).*?^\1[ \t]*$', re.MULTILINE | re.DOTALL)
HEADING = re.compile(r'^(#{1,6})(.*?)#*[ \t]*$', re.MULTILINE)
LINK = re.compile(r'\]\(([^()\s]+)\)')
# A content directory of gi-docgen other than docs/reference/aravis, in
# meson.build: '--content-dir=@0@'.format(meson.project_source_root() / 'gst')
EXTRA_CONTENT_DIR = re.compile(r"--content-dir=@0@'\.format\(\s*meson\.project_source_root\(\)((?:\s*/\s*'[^']*')+)\s*\)")


class Checks:
    """Collects the failed checks. Prints each check and note unless QUIET."""

    def __init__(self, quiet=False):
        self.failures = []
        self.quiet = quiet

    def check(self, condition, message):
        if not self.quiet:
            print(('ok: ' if condition else 'FAIL: ') + message)
        if not condition:
            self.failures.append(message)
        return condition

    def note(self, message):
        if not self.quiet:
            print(message)


def read(*path):
    with open(os.path.join(*path), encoding='utf-8') as f:
        return f.read()


def string_list(checks, text, name, what):
    """The file names of the list that NAME is set to in TEXT: name = [ ... ].
    An item is a string, or, for a page kept outside docs/reference/aravis, a
    meson path expression such as meson.project_source_root() / 'gst' /
    'pipelines.md', whose last string is the file name. Comments are ignored."""
    match = re.search(r'^\s*' + re.escape(name) + r'\s*=\s*\[(.*?)\]', text, re.MULTILINE | re.DOTALL)
    if not checks.check(match is not None, '{} sets {}'.format(what, name)):
        return []
    names = []
    for item in re.sub(r'#[^\n]*', '', match.group(1)).split(','):
        strings = re.findall(r"'([^']*)'", item)
        if strings:
            names.append(strings[-1])
    return names


def content_dirs(source_dir, meson_text):
    """docs/reference/aravis, then the other content directories that
    meson.build passes to gi-docgen."""
    dirs = [os.path.join(source_dir, DOCS)]
    for match in EXTRA_CONTENT_DIR.finditer(meson_text):
        dirs.append(os.path.join(source_dir, *re.findall(r"'([^']*)'", match.group(1))))
    return dirs


def slugify(text):
    """The identifier that Python-Markdown's toc extension gives to a section
    titled TEXT (markdown.extensions.toc.slugify, with '-' as separator)."""
    text = unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode('ascii')
    text = re.sub(r'[^\w\s-]', '', text).strip().lower()
    return re.sub(r'[-\s]+', '-', text)


def heading_text(title):
    """The text of a section title, without its inline markup."""
    title = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', title)
    return title.replace('`', '').replace('*', '').strip()


def section_ids(text):
    """The identifiers of the sections of a page, as Python-Markdown makes them:
    a second section with the same title gets _1, a third _2."""
    ids = []
    for match in HEADING.finditer(FENCE.sub('', text)):
        identifier = slugify(heading_text(match.group(2)))
        unique, n = identifier, 0
        while unique in ids:
            n += 1
            unique = '{}_{}'.format(identifier, n)
        ids.append(unique)
    return ids


def markdown_ids(text):
    """The identifiers that Python-Markdown gives the sections of a page, with
    the extensions of gi-docgen that matter here, or None without it."""
    try:
        import markdown
    except ImportError:
        return None
    md = markdown.Markdown(extensions=['fenced_code', 'meta', 'tables', 'toc'])
    return re.findall(r'<h[1-6] id="([^"]+)"', md.convert(text))


def html_name(page):
    return page[:-len('.md')] + '.html'


def find_pages(docs_dir):
    """The pages of the documentation, and the Markdown files that could not be
    read, with the error. Hidden files, such as the lock files of editors, are
    not pages."""
    pages = []
    unreadable = []
    for name in sorted(os.listdir(docs_dir)):
        if not name.endswith('.md') or name.startswith('.'):
            continue
        try:
            text = read(docs_dir, name)
        except (OSError, UnicodeDecodeError) as error:
            unreadable.append((name, error))
            continue
        if text.startswith('Title:'):
            pages.append(name)
    return pages, unreadable


def link_exists(sections, page, section):
    """Whether PAGE, and its SECTION unless None, exist: SECTIONS maps each page
    of the documentation to the identifiers of its sections. A page that
    gi-docgen generates for the API exists if its name has the form of theirs."""
    if page in sections:
        return section is None or section in sections[page]
    return API_PAGE.match(page) is not None


def page_links(docs_dir, pages, sections, page_dirs=None):
    """The relative links of the pages, and those that name no existing page or
    section. PAGE_DIRS maps a page kept outside DOCS_DIR to its directory."""
    links = []
    broken = []
    for page in pages:
        for target in LINK.findall(FENCE.sub('', read((page_dirs or {}).get(page, docs_dir), page))):
            match = RELATIVE.match(target)
            if match is not None:
                links.append(target)
                if not link_exists(sections, match.group(1) or html_name(page), match.group(2)):
                    broken.append('{}: {}'.format(page, target))
    return links, broken


def source_files(source_dir):
    for directory, directories, files in os.walk(source_dir):
        # Not the build directories, the subprojects and the version control
        directories[:] = sorted(d for d in directories
                                if d not in ('subprojects', '.git')
                                and not os.path.exists(os.path.join(directory, d, 'meson-private')))
        for name in sorted(files):
            if name.endswith(SOURCE_EXTENSIONS):
                yield os.path.join(directory, name)


def source_links(source_dir, sections):
    """The links of the files of the sources to the published documentation,
    those that name no existing page or section, and the files that could not
    be read, with the error, such as the lock files of Emacs, which are dangling
    symbolic links."""
    links = []
    broken = []
    unreadable = []
    for path in source_files(source_dir):
        relative = os.path.relpath(path, source_dir)
        try:
            text = read(path)
        except UnicodeDecodeError:
            # Not text
            continue
        except OSError as error:
            unreadable.append((relative, error))
            continue
        for match in PUBLISHED.finditer(text):
            links.append(match.group(0))
            if not link_exists(sections, match.group(1), match.group(2)):
                broken.append('{}: {}'.format(relative, match.group(0)))
    return links, broken, unreadable


def check_sources(checks, source_dir, compare=True):
    docs_dir = os.path.join(source_dir, DOCS)
    meson_text = read(docs_dir, 'meson.build')

    page_dirs = {}
    for directory in content_dirs(source_dir, meson_text):
        found, unreadable = find_pages(directory)
        for name, error in unreadable:
            checks.note('skipped: {}, which cannot be read ({})'.format(name, error))
        for page in found:
            if page in page_dirs:
                checks.check(False, 'the page {} is in one content directory only'.format(page))
            else:
                page_dirs[page] = directory
    pages = sorted(page_dirs)
    published = string_list(checks, read(docs_dir, 'aravis.toml.in'), 'content_files', 'aravis.toml.in')
    dependencies = string_list(checks, meson_text, 'expand_content_md_files', 'meson.build')
    for what, names in (('content_files of aravis.toml.in', published),
                        ('expand_content_md_files of meson.build', dependencies)):
        checks.check(len(set(names)) == len(names), 'the {} name each page once'.format(what))
        missing = sorted(set(pages) - set(names))
        checks.check(not missing, 'the {} name every page{}'.format(
            what, ' (missing: {})'.format(', '.join(missing)) if missing else ''))
        unknown = sorted(set(names) - set(pages))
        checks.check(not unknown, 'the {} name existing pages only{}'.format(
            what, ' (unknown: {})'.format(', '.join(unknown)) if unknown else ''))

    sections = {}
    for page in pages:
        text = read(page_dirs[page], page)
        ids = section_ids(text)
        sections[html_name(page)] = set(ids)
        reference = markdown_ids(text) if compare else None
        if reference is not None:
            checks.check(ids == reference, 'the section identifiers of {} are those of Python-Markdown'.format(page))
    if compare and markdown_ids('') is None:
        checks.note('skipped: comparison with Python-Markdown, which is not installed')

    links, broken = page_links(docs_dir, pages, sections, page_dirs)
    checks.check(links and not broken, 'the {} links between the pages name existing pages and sections{}'.format(
        len(links), ' (broken: {})'.format(', '.join(broken)) if broken else ''))

    links, broken, unreadable = source_links(source_dir, sections)
    for name, error in unreadable:
        checks.note('skipped: {}, which cannot be read ({})'.format(name, error))
    checks.check(not broken, 'the {} links of the sources to the published documentation name existing pages and '
                 'sections{}'.format(len(links), ' (broken: {})'.format(', '.join(broken)) if broken else ''))


# A made-up source tree for the self-test: two pages, a page without a Title:
# line, links, good and broken, between the pages and from a source file, a
# lock file of Emacs, a folder named like a page and a file that cannot be read
SELF_TEST_FILES = {
    'aravis.toml.in': "content_files = [\n  'a.md',\n  'b.md',\n]\n",
    'meson.build': "expand_content_md_files = [\n  'a.md',\n  'b.md',\n]\n",
    'a.md': ('Title: Page A\n\n# Page A\n\n## First section\n\n'
             'Good: [B](b.html), [B](b.html#second-section), [here](#first-section),\n'
             '[a class](class.Camera.html), [its methods](class.Camera.html#methods), [the API](index.html),\n'
             '[a method](method.Camera.uv_set_usb_mode.html), [a signal](signal.Device.control-lost.html),\n'
             '[a function](func.make_thread_realtime.html), [the classes](classes_hierarchy.html).\n\n'
             'Broken: [no section](b.html#missing), [a period](b.html#second-section.), [no page](c.html),\n'
             '[no API page](classes.Camera.html), [not here](#second-section).\n'),
    'b.md': 'Title: Page B\n\n# Page B\n\n## Second section\n\n```\n# Not a section\n[a](#not-a-link)\n```\n',
    'notes.md': '# Notes\n\nNot a page: no Title: line.\n',
}
SELF_TEST_SOURCE = ('/* See {0}a.html#first-section. The API: {0}class.Camera.html, {0}index.html,\n'
                    ' * {0}method.Camera.uv_set_usb_mode.html; and {0}b.html#second-section, or {0}b.html.\n'
                    ' * Broken: {0}a.html#missing, {0}c.html, {0}classes.Camera.html, {0}b.html#first-section. */\n'
                    ).format(PUBLISHED_BASE)
SELF_TEST_PAGE_LINKS = 15
SELF_TEST_BROKEN_PAGE_LINKS = ['a.md: b.html#missing', 'a.md: b.html#second-section.', 'a.md: c.html',
                               'a.md: classes.Camera.html', 'a.md: #second-section']
SELF_TEST_SOURCE_LINKS = 10
SELF_TEST_BROKEN_SOURCE_LINKS = [os.path.join('src', 'links.c') + ': ' + PUBLISHED_BASE + target
                                 for target in ('a.html#missing', 'c.html', 'classes.Camera.html',
                                                'b.html#first-section')]
SELF_TEST_UNREADABLE = [os.path.join(DOCS, '.#a.md'), os.path.join('src', 'unreadable.c')]


def add_unreadable_file(path):
    """Makes PATH a file that this process cannot read: a dangling symbolic link,
    such as the lock files of Emacs, or on Windows, where making a symbolic link
    needs a privilege, a file with a lock on its bytes. Returns the function that
    releases the lock."""
    if sys.platform != 'win32':
        os.symlink('user@host.1234:1700000000', path)
        return lambda: None
    import msvcrt
    with open(path, 'w') as f:
        f.write('/* {}b.html#missing */\n'.format(PUBLISHED_BASE))
    f = open(path, 'r+b')
    msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1 << 20)

    def release():
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1 << 20)
        f.close()
    return release


def self_test(checks):
    directory = tempfile.mkdtemp(prefix='arv-doc-pages-')
    releases = []
    try:
        docs_dir = os.path.join(directory, DOCS)
        os.makedirs(os.path.join(docs_dir, 'folder.md'))
        os.makedirs(os.path.join(directory, 'src'))
        for name, text in SELF_TEST_FILES.items():
            with open(os.path.join(docs_dir, name), 'w', encoding='utf-8') as f:
                f.write(text)
        with open(os.path.join(directory, 'src', 'links.c'), 'w', encoding='utf-8') as f:
            f.write(SELF_TEST_SOURCE)
        releases.append(add_unreadable_file(os.path.join(docs_dir, '.#a.md')))
        releases.append(add_unreadable_file(os.path.join(directory, 'src', 'unreadable.c')))

        pages, unreadable = find_pages(docs_dir)
        unreadable = [name for name, error in unreadable]
        checks.check(pages == ['a.md', 'b.md'] and unreadable == ['folder.md'],
                     'self-test: the pages are the Markdown files with a Title: line, without the hidden files, '
                     'and a folder named like a page is left out (pages: {}, cannot be read: {})'.format(
                         pages, unreadable))
        sections = {html_name(page): set(section_ids(read(docs_dir, page))) for page in pages}
        links, broken = page_links(docs_dir, pages, sections)
        checks.check(len(links) == SELF_TEST_PAGE_LINKS and broken == SELF_TEST_BROKEN_PAGE_LINKS,
                     'self-test: of the links between the pages, those to a missing page or section are reported, '
                     'and those to the API pages are accepted ({} links, broken: {})'.format(len(links), broken))
        links, broken, unreadable = source_links(directory, sections)
        unreadable = [name for name, error in unreadable]
        checks.check(len(links) == SELF_TEST_SOURCE_LINKS and broken == SELF_TEST_BROKEN_SOURCE_LINKS
                     and unreadable == SELF_TEST_UNREADABLE,
                     'self-test: of the links of the sources, those to a missing page or section are reported, '
                     'and a "." that ends a sentence is not part of a link; the files that cannot be read are left '
                     'out ({} links, broken: {}, cannot be read: {})'.format(len(links), broken, unreadable))

        # A page kept in another content directory of meson.build, listed through a path expression, is checked like
        # the others, and a page of that directory missing from the lists is reported
        extra = tempfile.mkdtemp(prefix='arv-doc-pages-extra-')
        try:
            os.makedirs(os.path.join(extra, DOCS))
            os.makedirs(os.path.join(extra, 'gst'))
            files = {
                os.path.join(DOCS, 'aravis.toml.in'): "content_files = [\n  'a.md',\n  'p.md',\n]\n",
                os.path.join(DOCS, 'meson.build'): ("expand_content_md_files = [\n  'a.md',\n"
                                                    "  # Kept with its sources\n"
                                                    "  meson.project_source_root() / 'gst' / 'p.md'\n]\n"
                                                    "args = ['--content-dir=@0@'.format(meson.current_source_dir()),\n"
                                                    "        '--content-dir=@0@'.format(meson.project_source_root() / 'gst')]\n"),
                os.path.join(DOCS, 'a.md'): 'Title: Page A\n\n# Page A\n\nSee [P](p.html#plugin).\n',
                os.path.join('gst', 'p.md'): 'Title: Page P\n\n# Plugin\n',
                os.path.join('gst', 'q.md'): 'Title: Page Q\n\n# Unlisted\n',
            }
            for name, text in files.items():
                with open(os.path.join(extra, name), 'w', encoding='utf-8') as f:
                    f.write(text)
            inner = Checks(quiet=True)
            check_sources(inner, extra, compare=False)
            checks.check(inner.failures == ['the content_files of aravis.toml.in name every page (missing: q.md)',
                                            'the expand_content_md_files of meson.build name every page '
                                            '(missing: q.md)'],
                         'self-test: a page of another content directory of meson.build is checked like the others, '
                         'and listed through a path expression ({})'.format(inner.failures))
        finally:
            shutil.rmtree(extra, ignore_errors=True)

        # The whole check of the tree fails on its broken links, and only on them
        inner = Checks(quiet=True)
        check_sources(inner, directory, compare=False)
        checks.check([failure.split(' (broken: ')[0] for failure in inner.failures] ==
                     ['the {} links between the pages name existing pages and sections'.format(SELF_TEST_PAGE_LINKS),
                      'the {} links of the sources to the published documentation name existing pages and '
                      'sections'.format(SELF_TEST_SOURCE_LINKS)],
                     'self-test: the check of a made-up tree fails on its broken links only')
    finally:
        for release in releases:
            release()
        shutil.rmtree(directory, ignore_errors=True)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    checks = Checks()
    self_test(checks)
    check_sources(checks, sys.argv[1])
    if checks.failures:
        print('{} check(s) failed'.format(len(checks.failures)))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
