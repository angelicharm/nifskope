#include "bsshape.h"
#include "settings.h"

#include "glscene.h"
#include "material.h"


BSShape::BSShape( Scene * s, const QModelIndex & b ) : Shape( s, b )
{
	
}

void BSShape::clear()
{
	Node::clear();

	iSkin = iSkinData = iSkinPart = QModelIndex();
	bssp = nullptr;
	bslsp = nullptr;
	bsesp = nullptr;

	verts.clear();
	norms.clear();
	tangents.clear();
	bitangents.clear();
	triangles.clear();
	coords.clear();
	colors.clear();
	bones.clear();
	weights.clear();
}

void BSShape::update( const NifModel * nif, const QModelIndex & index )
{
	Node::update( nif, index );

	if ( !iBlock.isValid() || !index.isValid() )
		return;

	if ( iBlock != index && iSkin != index && iSkinData != index && !nif->inherits( index, "NiProperty" ) )
		return;

	nifVersion = nif->getUserVersion2();

	auto vertexFlags = nif->get<quint16>( iBlock, "VF" );
	bool isDynamic = nif->inherits( iBlock, "BSDynamicTriShape" );

	bool isDataOnSkin = false;
	bool isSkinned = vertexFlags & 0x400;
	if ( nifVersion == 130 ) {
		skinInstName = "BSSkin::Instance";
		skinDataName = "BSSkin::BoneData";
	} else {
		skinInstName = "NiSkinInstance";
		skinDataName = "NiSkinData";
	}

	iSkin = nif->getBlock( nif->getLink( nif->getIndex( iBlock, "Skin" ) ), skinInstName );
	if ( !iSkin.isValid() )
		isSkinned = false;

	if ( isSkinned ) {
		iSkinData = nif->getBlock( nif->getLink( iSkin, "Data" ), skinDataName );
		iSkinPart = nif->getBlock( nif->getLink( iSkin, "Skin Partition" ), "NiSkinPartition" );

		if ( nifVersion == 100 )
			isDataOnSkin = true;
	}

	updateData = true;
	updateBounds |= updateData;
	updateSkin = isSkinned;
	transformRigid = !isSkinned;

	int dataSize = 0;
	if ( !isDataOnSkin ) {
		iVertData = nif->getIndex( iBlock, "Vertex Data" );
		iTriData = nif->getIndex( iBlock, "Triangles" );
		iData = iVertData;
		if ( !iVertData.isValid() || !iTriData.isValid() )
			return;

		numVerts = std::min( nif->get<int>( iBlock, "Num Vertices" ), nif->rowCount( iVertData ) );
		numTris = std::min( nif->get<int>( iBlock, "Num Triangles" ), nif->rowCount( iTriData ) );

		dataSize = nif->get<int>( iBlock, "Data Size" );
	} else {
		// For skinned geometry, the vertex data is stored on the NiSkinPartition
		// The triangles are split up among the partitions

		iSkinPart = nif->getBlock( nif->getLink( iSkin, "Skin Partition" ), "NiSkinPartition" );
		if ( !iSkinPart.isValid() )
			return;

		iVertData = nif->getIndex( iSkinPart, "Vertex Data" );
		iTriData = QModelIndex();
		iData = iVertData;

		dataSize = nif->get<int>( iSkinPart, "Data Size" );
		auto vertexSize = nif->get<int>( iSkinPart, "Vertex Size" );
		if ( !iVertData.isValid() || dataSize == 0 || vertexSize == 0 )
			return;

		numVerts = dataSize / vertexSize;
	}


	auto bsphere = nif->getIndex( iBlock, "Bounding Sphere" );
	if ( bsphere.isValid() ) {
		bsphereCenter = nif->get<Vector3>( bsphere, "Center" );
		bsphereRadius = nif->get<float>( bsphere, "Radius" );
	}

	if ( iBlock == index && dataSize > 0 ) {
		verts.clear();
		norms.clear();
		tangents.clear();
		bitangents.clear();
		triangles.clear();
		coords.clear();
		colors.clear();

		// For compatibility with coords QList
		QVector<Vector2> coordset;

		for ( int i = 0; i < numVerts; i++ ) {
			auto idx = nif->index( i, 0, iVertData );

			if ( !isDynamic )
				verts << nif->get<Vector3>( idx, "Vertex" );

			coordset << nif->get<HalfVector2>( idx, "UV" );

			// Bitangent X
			auto bitX = nif->getValue( nif->getIndex( idx, "Bitangent X" ) ).toFloat();
			// Bitangent Y/Z
			auto bitYi = nif->getValue( nif->getIndex( idx, "Bitangent Y" ) ).toCount();
			auto bitZi = nif->getValue( nif->getIndex( idx, "Bitangent Z" ) ).toCount();
			auto bitY = (double( bitYi ) / 255.0) * 2.0 - 1.0;
			auto bitZ = (double( bitZi ) / 255.0) * 2.0 - 1.0;

			norms += nif->get<ByteVector3>( idx, "Normal" );
			tangents += nif->get<ByteVector3>( idx, "Tangent" );
			bitangents += Vector3( bitX, bitY, bitZ );

			auto vcIdx = nif->getIndex( idx, "Vertex Colors" );
			if ( vcIdx.isValid() ) {
				colors += nif->get<ByteColor4>( vcIdx );
			}
		}

		if ( isDynamic ) {
			auto dynVerts = nif->getArray<Vector4>( iBlock, "Vertices" );
			for ( const auto & v : dynVerts )
				verts << Vector3( v );
		}

		// Add coords as first set of QList
		coords.append( coordset );

		if ( !isDataOnSkin ) {
			triangles = nif->getArray<Triangle>( iTriData );
			triangles = triangles.mid( 0, numTris );
		} else {
			auto partIdx = nif->getIndex( iSkinPart, "Partition" );
			for ( int i = 0; i < nif->rowCount( partIdx ); i++ )
				triangles << nif->getArray<Triangle>( nif->index( i, 0, partIdx ), "Triangles" );
		}
	}

	// Update shaders from this mesh's shader property
	updateShaderProperties( nif );

	if ( bssp )
		isVertexAlphaAnimation = bssp->hasSF2( ShaderFlags::SLSF2_Tree_Anim );

	if ( isVertexAlphaAnimation ) {
		for ( int i = 0; i < colors.count(); i++ )
			colors[i].setRGBA( colors[i].red(), colors[i].green(), colors[i].blue(), 1.0 );
	}
}

QModelIndex BSShape::vertexAt( int idx ) const
{
	auto nif = static_cast<const NifModel *>(iBlock.model());
	if ( !nif )
		return QModelIndex();

	// Vertices are on NiSkinPartition in version 100
	auto blk = iBlock;
	if ( nifVersion < 130 && iSkinPart.isValid() ) {
		if ( nif->inherits( blk, "BSDynamicTriShape" ) )
			return nif->getIndex( blk, "Vertices" ).child( idx, 0 );

		blk = iSkinPart;
	}

	return nif->getIndex( nif->getIndex( blk, "Vertex Data" ).child( idx, 0 ), "Vertex" );
}


void BSShape::transform()
{
	if ( isHidden() )
		return;

	const NifModel * nif = static_cast<const NifModel *>(iBlock.model());
	if ( !nif || !iBlock.isValid() ) {
		clear();
		return;
	}	
	
	if ( updateData ) {
		updateData = false;
	}

	if ( updateSkin ) {
		updateSkin = false;
		doSkinning = false;

		bones.clear();
		weights.clear();
		partitions.clear();

		if ( iSkin.isValid() && iSkinData.isValid() ) {
			skeletonRoot = nif->getLink( iSkin, "Skeleton Root" );
			if ( nifVersion < 130 )
				skeletonTrans = Transform( nif, iSkinData );

			bones = nif->getLinkArray( iSkin, "Bones" );
			weights.fill( BoneWeights(), bones.count() );
			for ( int i = 0; i < bones.count(); i++ )
				weights[i].bone = bones[i];

			for ( int i = 0; i < numVerts; i++ ) {
				auto idx = nif->index( i, 0, iVertData );
				auto wts = nif->getArray<float>( idx, "Bone Weights" );
				auto bns = nif->getArray<quint8>( idx, "Bone Indices" );
				if ( wts.count() < 4 || bns.count() < 4 )
					continue;

				for ( int j = 0; j < 4; j++ ) {
					if ( wts[j] > 0.0 )
						weights[bns[j]].weights << VertexWeight( i, wts[j] );
				}
			}

			doSkinning = weights.count();
		}
	}

	Node::transform();
}

void BSShape::transformShapes()
{
	if ( isHidden() )
		return;

	const NifModel * nif = static_cast<const NifModel *>(iBlock.model());
	if ( !nif || !iBlock.isValid() ) {
		clear();
		return;
	}

	Node::transformShapes();

	transformRigid = true;

	if ( doSkinning && scene->options & Scene::DoSkinning ) {
		transformRigid = false;

		transVerts.resize( verts.count() );
		transVerts.fill( Vector3() );
		transNorms.resize( verts.count() );
		transNorms.fill( Vector3() );
		transTangents.resize( verts.count() );
		transTangents.fill( Vector3() );
		transBitangents.resize( verts.count() );
		transBitangents.fill( Vector3() );

		auto b = nif->getIndex( iSkinData, "Bone List" );
		for ( int i = 0; i < weights.count(); i++ )
			weights[i].setTransform( nif, b.child( i, 0 ) );

		Node * root = findParent( 0 );
		for ( const BoneWeights & bw : weights ) {
			Node * bone = root ? root->findChild( bw.bone ) : nullptr;
			if ( bone ) {
				Transform t = scene->view * bone->localTrans( 0 ) * bw.trans;
				for ( const VertexWeight & w : bw.weights ) {
					if ( w.vertex >= verts.count() )
						continue;

					transVerts[w.vertex] += t * verts[w.vertex] * w.weight;
					transNorms[w.vertex] += t.rotation * norms[w.vertex] * w.weight;
					transTangents[w.vertex] += t.rotation * tangents[w.vertex] * w.weight;
					transBitangents[w.vertex] += t.rotation * bitangents[w.vertex] * w.weight;
				}
			}
		}

		for ( int n = 0; n < verts.count(); n++ ) {
			transNorms[n].normalize();
			transTangents[n].normalize();
			transBitangents[n].normalize();
		}

		boundSphere = BoundSphere( transVerts );
		boundSphere.applyInv( viewTrans() );
		updateBounds = false;
	} else {
		transVerts = verts;
		transNorms = norms;
		transTangents = tangents;
		transBitangents = bitangents;
	}

}

void BSShape::drawShapes( NodeList * secondPass, bool presort )
{
	if ( isHidden() )
		return;

	glPointSize( 8.5 );

	// TODO: Only run this if BSXFlags has "EditorMarkers present" flag
	if ( !(scene->options & Scene::ShowMarkers) && name.startsWith( "EditorMarker" ) )
		return;

	if ( Node::SELECTING ) {
		if ( scene->selMode & Scene::SelObject ) {
			int s_nodeId = ID2COLORKEY( nodeId );
			glColor4ubv( (GLubyte *)&s_nodeId );
		} else {
			glColor4f( 0, 0, 0, 1 );
		}
	}

	// Draw translucent meshes in second pass
	AlphaProperty * aprop = findProperty<AlphaProperty>();
	Material * mat = (bssp) ? bssp->mat() : nullptr;

	drawSecond |= aprop && aprop->blend();
	drawSecond |= mat && mat->bDecal;

	if ( secondPass && drawSecond ) {
		secondPass->add( this );
		return;
	}

	if ( transformRigid ) {
		glPushMatrix();
		glMultMatrix( viewTrans() );
	}

	// Render polygon fill slightly behind alpha transparency and wireframe
	if ( !drawSecond ) {
		glEnable( GL_POLYGON_OFFSET_FILL );
		glPolygonOffset( 1.0f, 2.0f );
	}

	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 3, GL_FLOAT, 0, transVerts.data() );

	if ( !Node::SELECTING ) {
		glEnableClientState( GL_NORMAL_ARRAY );
		glNormalPointer( GL_FLOAT, 0, transNorms.data() );

		bool doVCs = (bssp && (bssp->getFlags2() & ShaderFlags::SLSF2_Vertex_Colors));


		Color4 * c = nullptr;
		if ( colors.count() && (scene->options & Scene::DoVertexColors) && doVCs ) {
			c = colors.data();
		}

		if ( c ) {
			glEnableClientState( GL_COLOR_ARRAY );
			glColorPointer( 4, GL_FLOAT, 0, c );
		} else {
			glColor( Color3( 1.0f, 1.0f, 1.0f ) );
		}
	}


	if ( !Node::SELECTING )
		shader = scene->renderer->setupProgram( this, shader );
	
	if ( isDoubleSided ) {
		glCullFace( GL_FRONT );
		glDrawElements( GL_TRIANGLES, triangles.count() * 3, GL_UNSIGNED_SHORT, triangles.data() );
		glCullFace( GL_BACK );
	}

	glDrawElements( GL_TRIANGLES, triangles.count() * 3, GL_UNSIGNED_SHORT, triangles.data() );

	if ( !Node::SELECTING )
		scene->renderer->stopProgram();

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_NORMAL_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );

	glDisable( GL_POLYGON_OFFSET_FILL );


	if ( scene->selMode & Scene::SelVertex ) {
		drawVerts();
	}

	if ( transformRigid )
		glPopMatrix();
}

void BSShape::drawVerts() const
{
	glDisable( GL_LIGHTING );
	glNormalColor();

	glBegin( GL_POINTS );

	for ( int i = 0; i < numVerts; i++ ) {
		if ( Node::SELECTING ) {
			int id = ID2COLORKEY( ( shapeNumber << 16 ) + i );
			glColor4ubv( (GLubyte *)&id );
		}
		glVertex( transVerts.value( i ) );
	}

	auto nif = static_cast<const NifModel *>(iBlock.model());
	if ( !nif )
		return;

	// Vertices are on NiSkinPartition in version 100
	bool selected = iBlock == scene->currentBlock;
	if ( nifVersion < 130 && iSkinPart.isValid() ) {
		selected |= iSkinPart == scene->currentBlock;
		selected |= nif->inherits( iBlock, "BSDynamicTriShape" );
	}


	// Highlight selected vertex
	if ( !Node::SELECTING && selected ) {
		auto idx = scene->currentIndex;
		auto n = idx.data( Qt::DisplayRole ).toString();
		if ( n == "Vertex" || n == "Vertices" ) {
			glHighlightColor();
			glVertex( transVerts.value( idx.parent().row() ) );
		}
	}

	glEnd();
}

void BSShape::drawSelection() const
{
	if ( scene->options & Scene::ShowNodes )
		Node::drawSelection();

	if ( isHidden() || !(scene->selMode & Scene::SelObject) )
		return;

	auto idx = scene->currentIndex;
	auto blk = scene->currentBlock;

	// Is the current block extra data
	bool extraData = false;

	auto nif = static_cast<const NifModel *>(idx.model());
	if ( !nif )
		return;

	// Set current block name and detect if extra data
	auto blockName = nif->getBlockName( blk );
	if ( blockName == "BSPackedCombinedSharedGeomDataExtra" )
		extraData = true;

	// Don't do anything if this block is not the current block
	//	or if there is not extra data
	if ( blk != iBlock && blk != iSkin && blk != iSkinData && blk != iSkinPart && !extraData )
		return;

	// Name of this index
	auto n = idx.data( NifSkopeDisplayRole ).toString();
	// Name of this index's parent
	auto p = idx.parent().data( NifSkopeDisplayRole ).toString();
	// Parent index
	auto pBlock = nif->getBlock( nif->getParent( blk ) );

	auto push = [this] ( const Transform & t ) {	
		if ( transformRigid ) {
			glPushMatrix();
			glMultMatrix( t );
		}
	};

	auto pop = [this] () {
		if ( transformRigid )
			glPopMatrix();
	};

	push( viewTrans() );

	glDepthFunc( GL_LEQUAL );

	glDisable( GL_LIGHTING );
	glDisable( GL_COLOR_MATERIAL );
	glDisable( GL_TEXTURE_2D );
	glDisable( GL_NORMALIZE );
	glEnable( GL_DEPTH_TEST );
	glDepthMask( GL_FALSE );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	glDisable( GL_ALPHA_TEST );

	glDisable( GL_CULL_FACE );

	// TODO: User Settings
	int lineWidth = 1.5;
	int pointSize = 5.0;

	glLineWidth( lineWidth );
	glPointSize( pointSize );

	glNormalColor();

	glEnable( GL_POLYGON_OFFSET_FILL );
	glPolygonOffset( -1.0f, -2.0f );

	float normalScale = bounds().radius / 20;
	normalScale /= 2.0f;

	if ( normalScale < 0.1f )
		normalScale = 0.1f;

	

	// Draw All Verts lambda
	auto allv = [this]( float size ) {
		glPointSize( size );
		glBegin( GL_POINTS );

		for ( int j = 0; j < transVerts.count(); j++ )
			glVertex( transVerts.value( j ) );

		glEnd();
	};

	if ( n == "Bounding Sphere" && !extraData ) {
		auto sph = BoundSphere( nif, idx );
		if ( sph.radius > 0.0 ) {
			glColor4f( 1, 1, 1, 0.33 );
			drawSphereSimple( sph.center, sph.radius, 72 );
		}
	}
	
	if ( blockName == "BSPackedCombinedSharedGeomDataExtra" && pBlock == iBlock ) {
		QVector<QModelIndex> idxs;
		if ( n == "Bounding Sphere" ) {
			idxs += idx;
		} else if ( n == "BSPackedCombinedSharedGeomDataExtra" ) {
			auto data = nif->getIndex( idx, "Object Data" );
			int dataCt = nif->rowCount( data );

			for ( int i = 0; i < dataCt; i++ ) {
				auto d = data.child( i, 0 );

				int numC = nif->get<int>( d, "Num Combined" );
				auto c = nif->getIndex( d, "Combined" );
				int cCt = nif->rowCount( c );

				for ( int j = 0; j < cCt; j++ ) {
					idxs += nif->getIndex( c.child( j, 0 ), "Bounding Sphere" );
				}
			}
		}

		if ( !idxs.count() ) {
			glPopMatrix();
			return;
		}

		Vector3 pTrans = nif->get<Vector3>( pBlock, "Translation" );
		auto iBSphere = nif->getIndex( pBlock, "Bounding Sphere" );
		Vector3 pbvC = nif->get<Vector3>( iBSphere.child( 0, 2 ) );
		float pbvR = nif->get<float>( iBSphere.child( 1, 2 ) );

		if ( pbvR > 0.0 ) {
			glColor4f( 0, 1, 0, 0.33 );
			drawSphereSimple( pbvC, pbvR, 72 );
		}

		glPopMatrix();

		for ( auto i : idxs ) {
			Matrix mat = nif->get<Matrix>( i.parent(), "Rotation" );
			//auto trans = nif->get<Vector3>( idx.parent(), "Translation" );
			float scale = nif->get<float>( i.parent(), "Scale" );

			Vector3 bvC = nif->get<Vector3>( i, "Center" );
			float bvR = nif->get<float>( i, "Radius" );

			Transform t;
			t.rotation = mat.inverted();
			t.translation = bvC;
			t.scale = scale;

			glPushMatrix();
			glMultMatrix( scene->view * t );

			if ( bvR > 0.0 ) {
				glColor4f( 1, 1, 1, 0.33 );
				drawSphereSimple( Vector3( 0, 0, 0 ), bvR, 72 );
			}

			glPopMatrix();
		}

		glPushMatrix();
		glMultMatrix( viewTrans() );
	}

	if ( n == "Vertex Data" || n == "Vertex" || n == "Vertices" ) {
		allv( 5.0 );

		int s = -1;
		if ( (n == "Vertex Data" && p == "Vertex Data")
			 || (n == "Vertices" && p == "Vertices") ) {
			s = idx.row();
		} else if ( n == "Vertex" ) {
			s = idx.parent().row();
		}

		if ( s >= 0 ) {
			glPointSize( 10 );
			glDepthFunc( GL_ALWAYS );
			glHighlightColor();
			glBegin( GL_POINTS );
			glVertex( transVerts.value( s ) );
			glEnd();
		}
	} 
	
	glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

	// Draw Lines lambda
	auto lines = [this, &normalScale, &allv, &lineWidth]( const QVector<Vector3> & v ) {
		allv( 7.0 );

		int s = scene->currentIndex.parent().row();
		glBegin( GL_LINES );

		for ( int j = 0; j < transVerts.count() && j < v.count(); j++ ) {
			glVertex( transVerts.value( j ) );
			glVertex( transVerts.value( j ) + v.value( j ) * normalScale * 2 );
			glVertex( transVerts.value( j ) );
			glVertex( transVerts.value( j ) - v.value( j ) * normalScale / 2 );
		}

		glEnd();

		if ( s >= 0 ) {
			glDepthFunc( GL_ALWAYS );
			glHighlightColor();
			glLineWidth( 3.0 );
			glBegin( GL_LINES );
			glVertex( transVerts.value( s ) );
			glVertex( transVerts.value( s ) + v.value( s ) * normalScale * 2 );
			glVertex( transVerts.value( s ) );
			glVertex( transVerts.value( s ) - v.value( s ) * normalScale / 2 );
			glEnd();
			glLineWidth( lineWidth );
		}
	};
	
	// Draw Normals
	if ( n.contains( "Normal" ) ) {
		lines( transNorms );
	}

	// Draw Tangents
	if ( n.contains( "Tangent" ) ) {
		lines( transTangents );
	}

	// Draw Triangles
	if ( n == "Triangles" ) {
		int s = scene->currentIndex.row();
		if ( s >= 0 ) {
			glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
			glHighlightColor();

			Triangle tri = triangles.value( s );
			glBegin( GL_TRIANGLES );
			glVertex( transVerts.value( tri.v1() ) );
			glVertex( transVerts.value( tri.v2() ) );
			glVertex( transVerts.value( tri.v3() ) );
			glEnd();
			glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		}
	}

	// Draw Segments/Subsegments
	if ( n == "Segment" || n == "Sub Segment" || n == "Num Primitives" ) {
		auto sidx = idx;
		int s;
		if ( n != "Num Primitives" ) {
			sidx = idx.child( 1, 0 );
		}
		s = sidx.row();

		auto nif = static_cast<const NifModel *>(sidx.model());

		auto off = sidx.sibling( s - 1, 2 ).data().toInt() / 3;
		auto cnt = sidx.sibling( s, 2 ).data().toInt();

		auto numRec = sidx.sibling( s + 2, 2 ).data().toInt();

		QVector<QColor> cols = { { 255, 0, 0, 128 }, { 0, 255, 0, 128 }, { 0, 0, 255, 128 }, { 255, 255, 0, 128 },
								{ 0, 255, 255, 128 }, { 255, 0, 255, 128 }, { 255, 255, 255, 128 } 
		};

		glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

		int maxTris = triangles.count();

		if ( numRec > 0 ) {
			auto recs = sidx.sibling( s + 3, 0 );
			for ( int i = 0; i < numRec; i++ ) {
				auto subrec = recs.child( i, 0 );
				auto off = subrec.child( 0, 2 ).data().toInt() / 3;
				auto cnt = subrec.child( 1, 2 ).data().toInt();

				int j = off;
				for ( j; j < cnt + off; j++ ) {
					if ( j >= maxTris )
						continue;

					glColor( Color4(cols.value( i % 7 )) );
					Triangle tri = triangles[j];
					glBegin( GL_TRIANGLES );
					glVertex( transVerts.value( tri.v1() ) );
					glVertex( transVerts.value( tri.v2() ) );
					glVertex( transVerts.value( tri.v3() ) );
					glEnd();
				}
			}
		} else {
			glColor( Color4(cols.value( idx.row() % 7 )) );

			int i = off;
			for ( i; i < cnt + off; i++ ) {
				if ( i >= maxTris )
					continue;

				Triangle tri = triangles[i];
				glBegin( GL_TRIANGLES );
				glVertex( transVerts.value( tri.v1() ) );
				glVertex( transVerts.value( tri.v2() ) );
				glVertex( transVerts.value( tri.v3() ) );
				glEnd();
			}
		}
		pop();
		return;
	}

	// Draw all bones' bounding spheres
	if ( n == "NiSkinData" || n == "BSSkin::BoneData" ) {
		// Get shape block
		if ( nif->getBlock( nif->getParent( nif->getParent( blk ) ) ) == iBlock ) {
			auto bones = nif->getIndex( blk, "Bone List" );
			int ct = nif->rowCount( bones );

			for ( int i = 0; i < ct; i++ ) {
				auto b = bones.child( i, 0 );
				boneSphere( nif, b );
			}
		}
		pop();
		return;
	}

	// Draw bone bounding sphere
	if ( n == "Bone List" ) {
		if ( nif->isArray( idx ) ) {
			for ( int i = 0; i < nif->rowCount( idx ); i++ )
				boneSphere( nif, idx.child( i, 0 ) );
		} else {
			boneSphere( nif, idx );
		}
	}

	// General wireframe
	if ( blk == iBlock && idx != iVertData && p != "Vertex Data" && p != "Vertices" ) {
		glLineWidth( 1.6f );
		glNormalColor();
		for ( const Triangle& tri : triangles ) {
			glBegin( GL_TRIANGLES );
			glVertex( transVerts.value( tri.v1() ) );
			glVertex( transVerts.value( tri.v2() ) );
			glVertex( transVerts.value( tri.v3() ) );
			glEnd();
		}
	}

	glDisable( GL_POLYGON_OFFSET_FILL );

	pop();
}

BoundSphere BSShape::bounds() const
{
	if ( updateBounds ) {
		updateBounds = false;
		if ( verts.count() ) {
			boundSphere = BoundSphere( verts );
		} else {
			boundSphere = BoundSphere( bsphereCenter, bsphereRadius );
		}
	}

	return worldTrans() * boundSphere;
}

bool BSShape::isHidden() const
{
	return Node::isHidden();
}
