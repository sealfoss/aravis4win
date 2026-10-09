; aravis-u3v.cdf: MakeCat definition of aravis-u3v.cat, the catalog of the
; aravis-u3v.inf driver package. In the folder of these two files, run
;
;     makecat aravis-u3v.cdf
;     signtool sign /fd SHA256 /sha1 <certificate thumbprint> aravis-u3v.cat
;
; MakeCat and SignTool come with the Windows SDK. Inf2Cat cannot make this catalog:
; its signability test B2.6.4.9 rejects a third-party USB driver that matches a
; compatible ID. CatalogVersion 2 hashes the INF with SHA-256, and the OSAttr
; attribute targets Windows 10 and later.

[CatalogHeader]
Name=aravis-u3v.cat
PublicVersion=0x00000001
EncodingType=0x00010001
CatalogVersion=2
HashAlgorithms=SHA256
CATATTR1=0x10010001:OSAttr:2:10.0

[CatalogFiles]
<hash>aravis-u3v.inf=aravis-u3v.inf
