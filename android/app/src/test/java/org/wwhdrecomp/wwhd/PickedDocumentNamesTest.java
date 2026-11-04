package org.wwhdrecomp.wwhd;
public final class PickedDocumentNamesTest {
    public static void main(String[] args) {
        assert PickedDocumentNames.fileName("Story Quests.wwhdmod", "msf:42").equals("Story Quests.wwhdmod");
        assert PickedDocumentNames.fileName("Common.pack", null).equals("Common.pack");  // a replacement pack keeps its name
        // no display name: the document ID's last part
        assert PickedDocumentNames.fileName(null, "primary:Download/My Mod.zip").equals("My Mod.zip");
        assert PickedDocumentNames.fileName("", "msf:42").equals("42");
        // never a path, a hidden file, "." or ".."
        assert PickedDocumentNames.fileName("../../evil.zip", null).equals("_.._evil.zip");
        assert PickedDocumentNames.fileName("a/b\\c:d.zip", null).equals("a_b_c_d.zip");
        assert PickedDocumentNames.fileName("..", null).equals(PickedDocumentNames.FALLBACK);
        assert PickedDocumentNames.fileName(".hidden.zip", null).equals("hidden.zip");
        assert PickedDocumentNames.fileName(null, null).equals(PickedDocumentNames.FALLBACK);
        assert PickedDocumentNames.fileName("bad\u0000name\n.zip", null).equals("bad_name_.zip");
        // long names are cut, the extension stays
        StringBuilder longName = new StringBuilder();
        for (int i = 0; i < 300; i++) longName.append('x');
        String cut = PickedDocumentNames.fileName(longName + ".wwhdmod", null);
        assert cut.length() == 120 && cut.endsWith(".wwhdmod") : cut;
        System.out.println("Picked document names: display name, document ID, unsafe and long names PASS");
    }
}
