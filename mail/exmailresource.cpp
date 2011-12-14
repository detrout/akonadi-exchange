/*
 * This file is part of the Akonadi Exchange Resource.
 * Copyright 2011 Robert Gruber <rgruber@users.sourceforge.net>
 *
 * Akonadi Exchange Resource is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Akonadi Exchange Resource is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Akonadi Exchange Resource.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include "exmailresource.h"

#include "settings.h"
#include "settingsadaptor.h"

#include <QtDBus/QDBusConnection>

#include <KLocalizedString>
#include <KWindowSystem>
#include <KStandardDirs>

#include <Akonadi/AgentManager>
#include <Akonadi/ItemFetchJob>
#include <Akonadi/ItemFetchScope>
#include <akonadi/kmime/messageparts.h>
#include <kmime/kmime_message.h>

#include "mapiconnector2.h"
#include "profiledialog.h"

using namespace Akonadi;

/**
 * We store all objects in Akonadi using the densest string representation to hand.
 */
static QString toStringId(qulonglong id)
{
	return QString::number(id, 36);
}

static qulonglong fromStringId(const QString &id)
{
	return id.toULongLong(0, 36);
}

/**
 * We store all objects in Akonadi using the densest string representation to hand.
 */
static QChar fidIdSeparator = QChar::fromAscii(':');
typedef QPair<qulonglong, qulonglong> FullId;
static QString toStringFullId(FullId &fullId)
{
	return QString::number(fullId.first, 36).append(fidIdSeparator).append(QString::number(fullId.second, 36));
}

static FullId fromStringFullId(const QString &id)
{
	FullId result;

	int separator = id.indexOf(fidIdSeparator);
	result.first = id.left(separator).toULongLong(0, 36);
	result.second = id.mid(separator + 1).toULongLong(0, 36);
	return result;
}

ExMailResource::ExMailResource(const QString &id) :
	ResourceBase(id),
	m_connection(new MapiConnector2()),
	m_connected(false)
{
	new SettingsAdaptor(Settings::self());
	QDBusConnection::sessionBus().registerObject(QLatin1String("/Settings"),
						     Settings::self(),
						     QDBusConnection::ExportAdaptors);
	if (name() == identifier()) {
		//setName(i18n("Exchange"));
	}

	setHierarchicalRemoteIdentifiersEnabled(true);
	//setCollectionStreamingEnabled(true);
	//setItemStreamingEnabled(true);
}

ExMailResource::~ExMailResource()
{
	logoff();
	delete m_connection;
}

void ExMailResource::error(const QString &message)
{
	kError() << message;
	emit status(Broken, message);
	cancelTask(message);
}

void ExMailResource::error(const MapiFolder &folder, const QString &body)
{
	static QString prefix = QString::fromAscii("Error %1: %2");
	QString message = prefix.arg(toStringId(folder.id())).arg(body);

	error(message);
}

void ExMailResource::error(const Akonadi::Collection &collection, const QString &body)
{
	static QString prefix = QString::fromAscii("Error %1(%2): %3");
	QString message = prefix.arg(collection.remoteId()).arg(collection.name()).arg(body);

	error(message);
}

void ExMailResource::error(const MapiMessage &msg, const QString &body)
{
	static QString prefix = QString::fromAscii("Error %1/%2: %3");
	QString message = prefix.arg(toStringId(msg.folderId())).arg(toStringId(msg.id())).arg(body);

	error(message);
}

void ExMailResource::retrieveCollections()
{
	kError() << "fetch collections";

	if (!logon()) {
		// Come back later.
		deferTask();
		return;
	}

	// create the new collection
	mapi_id_t rootId;
	if (!m_connection->defaultFolder(TopInformationStore, &rootId))
	{
		error(i18n("cannot find Exchange folder root"));
		return;
	}
	Collection::List collections;
	Collection root/*(rootId)*/;
	FullId remoteId(0, rootId);

	root.setName(name());
	root.setRemoteId(toStringFullId(remoteId));
	root.setParentCollection(Collection::root());
//	root.setResource(identifier());
	root.setContentMimeTypes(QStringList(Akonadi::Collection::mimeType()));
	root.setRights(Akonadi::Collection::ReadOnly);
	collections.append(root);
	if (collections.size() == 0) {
		error(i18n("no mail folders in Exchange"));
		return;
	}
	retrieveCollections(root.name(), root, QString::fromAscii(IPF_NOTE), collections);

	// notify akonadi about the new calendar collection
	collectionsRetrieved(collections);
}

void ExMailResource::retrieveCollections(const QString &path, const Collection &parent, const QString &filter, Akonadi::Collection::List &collections)
{
	kError() << "fetch collections in:" << path;

	QStringList contentTypes;
	contentTypes << KMime::Message::mimeType() << Akonadi::Collection::mimeType();

	FullId parentRemoteId = fromStringFullId(parent.remoteId());
	MapiFolder parentFolder(m_connection, "ExMailResource::retrieveCollection", parentRemoteId.second);
	if (!parentFolder.open()) {
		error(parentFolder, i18n("cannot open Exchange folder list"));
		return;
	}

	QList<MapiFolder *> list;
	emit status(Running, i18n("fetching folder list from Exchange: %1").arg(path));
	if (!parentFolder.childrenPull(list, filter)) {
		error(parentFolder, i18n("cannot fetch folder list from Exchange"));
		return;
	}

	QChar separator = QChar::fromAscii('/');
	foreach (MapiFolder *data, list) {
		Collection child/*(data->id())*/;
		FullId remoteId(parentRemoteId.second, data->id());

		child.setName(data->name);
		child.setRemoteId(toStringFullId(remoteId));
		child.setParentCollection(parent);
//		child.setResource(identifier());
		child.setContentMimeTypes(contentTypes);
		collections.append(child);

		// Recurse down...
		QString currentPath = path + separator + child.name();
		retrieveCollections(currentPath, child, filter, collections);
		delete data;
	}
}

void ExMailResource::retrieveItems(const Akonadi::Collection &collection)
{
	kError() << "fetch items from collection:" << collection.name();

	if (!logon()) {
		// Come back later.
		deferTask();
		return;
	}

	// find all item that are already in this collection
	QSet<FullId> knownRemoteIds;
	QMap<FullId, Item> knownItems;
	{
		emit status(Running, i18n("Fetching items from Akonadi cache"));
		ItemFetchJob *fetch = new ItemFetchJob( collection );

		Akonadi::ItemFetchScope scope;
		// we are only interested in the items from the cache
		scope.setCacheOnly(true);
		// we don't need the payload (we are mainly interested in the remoteID and the modification time)
		scope.fetchFullPayload(false);
		fetch->setFetchScope(scope);
		if ( !fetch->exec() ) {
			error(collection, i18n("unable to fetch listing of collection: %1", fetch->errorString()));
			return;
		}
		Item::List existingItems = fetch->items();
		foreach (Item item, existingItems) {
			// store all the items that we already know
			FullId remoteId = fromStringFullId(item.remoteId());
			knownRemoteIds.insert(remoteId);
			knownItems.insert(remoteId, item);
		}
	}
	kError() << "knownRemoteIds:" << knownRemoteIds.size();

	FullId parentRemoteId = fromStringFullId(collection.remoteId());
	MapiFolder parentFolder(m_connection, "ExMailResource::retrieveItems", parentRemoteId.second);
	if (!parentFolder.open()) {
		error(collection, i18n("unable to open collection"));
		return;
	}

	// get the folder content for the collection
	Item::List items;
	QList<MapiItem *> list;
	emit status(Running, i18n("Fetching collection: %1", collection.name()));
	if (!parentFolder.childrenPull(list)) {
		error(collection, i18n("unable to fetch collection"));
		return;
	}
	kError() << "fetched:" << list.size() << "items from collection:" << collection.name();

	QSet<FullId> checkedRemoteIds;
	// run though all the found data...
	foreach (MapiItem *data, list) {
		FullId remoteId(parentRemoteId.second, data->id());
		checkedRemoteIds << remoteId; // store for later use

		if (!knownRemoteIds.contains(remoteId)) {
			// we do not know this remoteID -> create a new empty item for it
			Item item(KMime::Message::mimeType());
			item.setParentCollection(collection);
			item.setRemoteId(toStringFullId(remoteId));
			item.setRemoteRevision(QString::number(1));
			items << item;
		} else {
			// this item is already known, check if it was update in the meanwhile
			Item& existingItem = knownItems.find(remoteId).value();
// 				kDebug() << "Item("<<existingItem.id()<<":"<<data.id<<":"<<existingItem.revision()<<") is already known [Cache-ModTime:"<<existingItem.modificationTime()
// 						<<" Server-ModTime:"<<data.modified<<"] Flags:"<<existingItem.flags()<<"Attrib:"<<existingItem.attributes();
			if (existingItem.modificationTime() < data->modified()) {
				kDebug() << existingItem.id()<<"=> this item has changed";

				// force akonadi to call retrieveItem() for this item in order to get updated data
				int revision = existingItem.remoteRevision().toInt();
				existingItem.clearPayload();
				existingItem.setRemoteRevision( QString::number(++revision) );
				items << existingItem;
			}
		}
		delete data;
		// TODO just for debugging...
			if (items.size() > 3) {
				break;
			}
	}

	// now check if some of the items need to be removed
	knownRemoteIds.subtract(checkedRemoteIds);

	Item::List deletedItems;
	foreach (const FullId &remoteId, knownRemoteIds) {
		deletedItems << knownItems.value(remoteId);
	}

	kError() << "itemsRetrievedIncremental(): fetched"<<items.size()<<"new/changed items; delete"<<deletedItems.size()<<"items";
	itemsRetrievedIncremental(items, deletedItems);

	foreach(Item item, items) {
		kDebug() << "[Item-Dump] ID:"<<item.id()<<"RemoteId:"<<item.remoteId()<<"Revision:"<<item.revision()<<"ModTime:"<<item.modificationTime();
	}

	// We fetched a load of stuff. This seems like a good place to force 
	// any subsequent activity to re-attempt the login.
	logoff();
}

bool ExMailResource::retrieveItem( const Akonadi::Item &itemOrig, const QSet<QByteArray> &parts )
{
	Q_UNUSED( parts );

	kError() << "fetch item" << itemOrig.id() << "remoteId:" << itemOrig.remoteId() <<
	itemOrig.remoteRevision() << itemOrig.parentCollection()<<"id"<<itemOrig.storageCollectionId();

	if (!logon()) {
		return false;
	}

	FullId id = fromStringFullId(itemOrig.remoteId());
	MapiNote message(m_connection, "ExMailResource::retrieveItem", id.first, id.second);
	if (!message.open()) {
		kError() << "open failed!";
		emit status(Broken, i18n("Unable to open item: { %1, %2 }", id.first, id.second));
		return false;
	}

	// find the remoteId of the item and the collection and try to fetch the needed data from the server
	kWarning() << "fetching item: {" <<
		currentCollection().name() << "," << itemOrig.id() << "} = {" <<
		itemOrig.remoteId() << "}";
	emit status(Running, i18n("Fetching item: { %1, %2 }", id.first, id.second));
	if (!message.propertiesPull()) {
		kError() << "propertiesPull failed!";
		emit status(Broken, i18n("Unable to fetch item: { %1, %2 }", id.first, id.second));
		return false;
	}
	kDebug() << "got message; item:"<<message.id()<<":" << message.title;

	// Create a clone of the passed in Item and fill it with the payload
	Akonadi::Item item(itemOrig);
#if 0
	KCal::Event* event = new KCal::Event;
	event->setUid(item.remoteId());
	event->setSummary( message.title );
	event->setDtStart( KDateTime(message.begin) );
	event->setDtEnd( KDateTime(message.end) );
	event->setCreated( KDateTime(message.created) );
	event->setLastModified( KDateTime(message.modified) );
	event->setDescription( message.text );
	//event->setOrganizer( message.sender );
	//event->setLocation( message.location );

	foreach (Attendee att, message.attendees) {
		if (att.isOrganizer()) {
			KCal::Person person(att.name, att.email);
			event->setOrganizer(person);
		} else {
			KCal::Attendee *person = new KCal::Attendee(att.name, att.email);
			event->addAttendee(person);
		}
	}
#endif

	// TODO add further message

	//item.setPayload( KCal::Incidence::Ptr(event) );

	item.setModificationTime(message.created);

	// notify akonadi about the new data
	itemRetrieved(item);
	return true;
}

void ExMailResource::aboutToQuit()
{
  // TODO: any cleanup you need to do while there is still an active
  // event loop. The resource will terminate after this method returns
}

void ExMailResource::configure( WId windowId )
{
	ProfileDialog dlgConfig(Settings::self()->profileName());
  	if (windowId)
		KWindowSystem::setMainWindow(&dlgConfig, windowId);

	if (dlgConfig.exec() == KDialog::Accepted) {

		QString profile = dlgConfig.getProfileName();
		Settings::self()->setProfileName( profile );
		Settings::self()->writeConfig();

		synchronize();
		emit configurationDialogAccepted();
	} else {
		emit configurationDialogRejected();
	}
}

void ExMailResource::itemAdded( const Akonadi::Item &item, const Akonadi::Collection &collection )
{
  Q_UNUSED( item );
  Q_UNUSED( collection );

  // TODO: this method is called when somebody else, e.g. a client application,
  // has created an item in a collection managed by your resource.

  // NOTE: There is an equivalent method for collections, but it isn't part
  // of this template code to keep it simple
}

/**
 * Called when somebody else, e.g. a client application, has changed an item 
 * managed this resource.
 */
void ExMailResource::itemChanged(const Akonadi::Item &item, const QSet<QByteArray> &parts)
{
        Q_UNUSED(parts);

        // Get the payload for the item.
	kWarning() << "fetch cached item: {" <<
		currentCollection().name() << "," << item.id() << "} = {" <<
		currentCollection().remoteId() << "," << item.remoteId() << "}";
        Akonadi::ItemFetchJob *job = new Akonadi::ItemFetchJob(item);
        connect(job, SIGNAL(result(KJob*)), SLOT(itemChangedContinue(KJob*)));
        job->fetchScope().fetchFullPayload();
}

/**
 * Finish changing an item, now that we (hopefully!) have its payload in hand.
 */
void ExMailResource::itemChangedContinue(KJob* job)
{
        if (job->error()) {
            emit status(Broken, i18n("Failed to get cached data"));
            return;
        }
        Akonadi::ItemFetchJob *fetchJob = qobject_cast<Akonadi::ItemFetchJob*>(job);
        const Akonadi::Item item = fetchJob->items().first();

	if (!logon()) {
		return;
	}

	qulonglong messageId = item.remoteId().toULongLong();
	qulonglong folderId = currentCollection().remoteId().toULongLong();
	MapiNote message(m_connection, "ExMailResource::itemChangedContinue", folderId, messageId);
	if (!message.open()) {
		kError() << "open failed!";
		emit status(Broken, i18n("Unable to open item: { %1, %2 }", currentCollection().name(), messageId));
		return;
	}

	// find the remoteId of the item and the collection and try to fetch the needed data from the server
	kWarning() << "fetching item: {" << 
		currentCollection().name() << "," << item.id() << "} = {" << 
		folderId << "," << messageId << "}";
	emit status(Running, i18n("Fetching item: { %1, %2 }", currentCollection().name(), messageId));
	if (!message.propertiesPull()) {
		kError() << "propertiesPull failed!";
		emit status(Broken, i18n("Unable to fetch item: { %1, %2 }", currentCollection().name(), messageId));
		return;
	}
        kWarning() << "got item data:" << message.id() << ":" /*<< message.title*/;

	// Extract the event from the item.
#if 0
	KCal::Event::Ptr event = item.payload<KCal::Event::Ptr>();
	Q_ASSERT(event->setUid == item.remoteId());
	message.title = event->summary();
	message.begin.setTime_t(event->dtStart().toTime_t());
	message.end.setTime_t(event->dtEnd().toTime_t());
	Q_ASSERT(message.created == event->created());

	// Check that between the item being modified, and this update attempt
	// that no conflicting update happened on the server.
	if (event->lastModified() < KDateTime(message.modified)) {
		kWarning() << "Exchange data modified more recently" << event->lastModified()
			<< "than cached data" << KDateTime(message.modified);
		// TBD: Update cache with data from Exchange.
		return;
	}
	message.modified = item.modificationTime();
	message.text = event->description();
	message.sender = event->organizer().name();
	message.location = event->location();
	if (event->alarms().count()) {
		KCal::Alarm* alarm = event->alarms().first();
		message.reminderActive = true;
		// TODO Maybe we should check which one is set and then use either the time or the delte
		// KDateTime reminder(message.reminderTime);
		// reminder.setTimeSpec( KDateTime::Spec(KDateTime::UTC) );
		// alarm->setTime( reminder );
		message.reminderMinutes = alarm->startOffset() / -60;
	} else {
		message.reminderActive = false;
	}

	message.attendees.clear();
	Attendee att;
	att.name = event->organizer().name();
	att.email = event->organizer().email();
	att.setOrganizer(true);
	message.attendees.append(att);
	att.setOrganizer(false);
	foreach (KCal::Attendee *person, event->attendees()) {
	att.name = person->name();
	att.email = person->email();
	message.attendees.append(att);
	}

	if (message.recurrency.isRecurring()) {
		// if this event is a recurring event create the recurrency
//                createKCalRecurrency(event->recurrence(), message.recurrency);
	}

	// TODO add further data

	// Update exchange with the new message.
	kWarning() << "updating item: {" << 
		currentCollection().name() << "," << item.id() << "} = {" << 
		folderId << "," << messageId << "}";
	emit status(Running, i18n("Updating item: { %1, %2 }", currentCollection().name(), messageId));
	if (!message.propertiesPush()) {
		kError() << "propertiesPush failed!";
		emit status(Running, i18n("Failed to update: { %1, %2 }", currentCollection().name(), messageId));
		return;
	}
	changeCommitted(item);
#endif
}

void ExMailResource::itemRemoved( const Akonadi::Item &item )
{
  Q_UNUSED( item );

  // TODO: this method is called when somebody else, e.g. a client application,
  // has deleted an item managed by your resource.

  // NOTE: There is an equivalent method for collections, but it isn't part
  // of this template code to keep it simple
}

bool ExMailResource::logon(void)
{
	if (!m_connected) {
		// logon to exchange (if needed)
		emit status(Running, i18n("Logging in to Exchange"));
		m_connected = m_connection->login(Settings::self()->profileName());
	}
	if (!m_connected) {
		emit status(Broken, i18n("Unable to login as %1").arg(Settings::self()->profileName()));
	}
	return m_connected;
}

void ExMailResource::logoff(void)
{
	// There is no logoff operation. We just want to make sure we retry the
	// logon next time.
	m_connected = false;
}

AKONADI_RESOURCE_MAIN( ExMailResource )

#include "exmailresource.moc"